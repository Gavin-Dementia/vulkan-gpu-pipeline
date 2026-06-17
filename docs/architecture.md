# Architecture Overview

This document describes the high-level architecture of the renderer.

> Superseded the original Phase-0-era version of this file (Instance/
> Surface/Device/Swapchain only). See `TECHNICAL_NOTES.md` for the
> decision-by-decision rationale behind each piece below.

---

## Current Architecture

```
Application
└── VulkanContext
    ├── initCore()              instance → device → swapchain → depth →
    │                            renderpass → pipeline → framebuffer
    ├── initSceneData()         OBJ load, vertex/index buffer, instance grid
    └── initCullingResources()  object buffer, visible-instance buffer,
                                 indirect draw buffer, compute descriptor/pipeline
        └── FrameRenderer
            └── FrameGraph (DAG, Kahn's algorithm)
                ├── GPUCullingPass   [Compute stage]
                └── GeometryPass     [Graphics stage, depends on CullingPass]
```

---

## Module Responsibilities

### Application

Responsible for:
- Application lifecycle
- Main loop, deltaTime computation
- Window ownership
- Driving `Camera::processInput()` each frame

Not responsible for:
- Vulkan resource management

### VulkanContext

Central manager of Vulkan subsystems. Split into three init phases to
avoid a single god-function (`init()` originally grew to ~150 lines
before this split — see `TECHNICAL_NOTES.md` §11):

- `initCore()` — instance, surface, device, swapchain, depth buffer,
  render pass, graphics pipeline, framebuffer
- `initSceneData()` — OBJ mesh loading + deduplication, vertex/index
  buffer upload, 7×7×7 instance grid generation
- `initCullingResources()` — object bounding-sphere buffer, visible-
  instance output buffer, indirect draw buffer, frustum uniform buffer,
  compute descriptor set, compute pipeline

### FrameRenderer

Owns per-frame synchronization (fence/semaphore pairs, double-buffered)
and the `FrameGraph` instance. `drawFrame()`:

1. Waits on the current frame's fence, acquires the next swapchain image
2. Records `executeCompute()` — runs outside any render pass
3. Inserts a memory barrier (compute write → vertex/indirect read)
4. Begins the render pass, records `executeGraphics()`, ends the render pass
5. Submits, presents

### FrameGraph

A DAG of render passes. Each `RGPass` declares:
- `name`
- `reads` — indices of passes this one depends on
- `stage` — `Compute` or `Graphics`
- `execute` — the actual command-recording lambda

`build()` resolves execution order via Kahn's algorithm (BFS topological
sort using indegree counts), throwing on cycle detection. `executeCompute()`
/ `executeGraphics()` walk the resolved order, filtering by stage.

### Camera

First-person controller (WASD + QE + arrow keys for yaw/pitch). Exposes
`getViewMatrix()`, called once per frame and shared identically by both
the culling compute pass (frustum construction) and the geometry pass
(vertex transform) — see `TECHNICAL_NOTES.md` §10 for why this single-
source-of-truth matters.

### ObjLoader

Loads OBJ via tinyobjloader, deduplicates vertices into a `unordered_map`
keyed by position+normal (Suzanne: 2904 → 507 unique vertices), returns
`{ vertices, indices }`.

### Compute culling pipeline

- `ObjectBuffer` (SSBO, read-only in shader) — per-instance bounding
  sphere `(center.xyz, radius)`
- `culling.comp` — 64 threads/workgroup, sphere-vs-6-plane frustum test;
  passing threads claim a unique slot via `atomicAdd` on the indirect
  draw buffer's `instanceCount` and write their instance data into
  `VisibleInstanceBuffer`
- `IndirectDrawBuffer` — `VkDrawIndexedIndirectCommand`, `instanceCount`
  written entirely by the GPU; reset to 0 by the CPU each frame before
  dispatch (not after — `atomicAdd` only increments)
- `vkCmdDrawIndexedIndirect` — CPU never reads back which instances
  passed culling or how many; it submits the indirect command and the
  GPU supplies the parameters

---

## Current Dependency Graph

```
Application
└── VulkanContext
    ├── VulkanInstance
    ├── VulkanSurface
    ├── VulkanDevice
    ├── VulkanSwapchain
    ├── VulkanDepthBuffer
    ├── VulkanRenderPass
    ├── VulkanPipeline (graphics)
    ├── VulkanComputePipeline
    ├── VulkanDescriptor (graphics, UBO binding 0)
    ├── ComputeDescriptor (compute, 4 bindings: object/visible/frustum/indirect)
    ├── VertexBuffer / IndexBuffer / InstanceBuffer
    ├── ObjectBuffer / VisibleInstanceBuffer / IndirectDrawBuffer / FrustumBuffer
    └── Camera
        └── FrameRenderer
            └── FrameGraph
                ├── GPUCullingPass (Compute)
                └── GeometryPass (Graphics)
```

---

## Frame pipeline (as implemented)

```
Acquire Swapchain Image
        │
Reset IndirectDrawBuffer.instanceCount = 0
        │
Compute: GPUCullingPass
   (sphere-frustum test, 343 threads, atomic compaction)
        │
Memory Barrier (SHADER_WRITE → VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ)
        │
Begin Render Pass
        │
Graphics: GeometryPass
   (bind vertex/index/visible-instance buffers,
    vkCmdDrawIndexedIndirect — GPU-supplied instance count)
        │
End Render Pass
        │
Queue Submit → Present
```

