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
    ├── initSceneData()         3× OBJ load (LOD0/1/2), vertex/index
    │                            buffer per LOD, 7×7×7 instance grid
    └── initCullingResources()  shared object buffer (343 bounding
                                 spheres), 3× {visible-instance buffer,
                                 indirect draw buffer} — one pair per
                                 LOD, frustum uniform buffer, compute
                                 descriptor set (8 bindings), compute
                                 pipeline
        └── FrameRenderer
            └── FrameGraph (DAG, Kahn's algorithm)
                ├── GPUCullingPass   [Compute stage] — frustum test +
                │                     distance-based LOD fan-out
                └── GeometryPass     [Graphics stage, depends on
                                       CullingPass] — 1 indexed-indirect
                                       draw per LOD
```

> The 3-LOD fan-out (one `{visible-instance, indirect draw}` pair per
> LOD, selected by camera distance inside the culling shader) replaced
> an earlier single-output design — see `TECHNICAL_NOTES.md` §15 for
> the pivot's rationale.

---

## Module Responsibilities

### Application

Responsible for:
- Application lifecycle
- Main loop, deltaTime computation
- Window ownership
- Driving `Camera::processInput()` each frame
- Polling (not callback-based) left-click input to launch `Projectile`,
  gated on `!ImGui::GetIO().WantCaptureMouse` — see `Projectile` below
  for why polling instead of a GLFW callback
- Polling Escape each frame to quit (alongside the existing
  `glfwWindowShouldClose` check)

Not responsible for:
- Vulkan resource management

### VulkanContext

Central manager of Vulkan subsystems. Split into three init phases to
avoid a single god-function (`init()` originally grew to ~150 lines
before this split — see `TECHNICAL_NOTES.md` §11):

- `initCore()` — instance, surface, device, swapchain, depth buffer,
  render pass, graphics pipeline, framebuffer, plus a second UBO +
  descriptor set (`projectileUniformBuffer_`/`projectileDescriptor_`)
  for the mouse-fired projectile — see `TECHNICAL_NOTES.md` §17 for why
  it can't share the grid's UBO — and a shared `sceneDataBuffer_`
  (light + camera data, `TECHNICAL_NOTES.md` §19) created *before* both
  descriptor sets, since both reference it at binding 2
- `initSceneData()` — OBJ mesh loading + deduplication for 3 LOD meshes
  (`suzanne.obj`, `suzanne_lod1.obj`, `suzanne_lod2.obj`), vertex/index
  buffer upload per LOD, 7×7×7 instance grid generation, plus a 1-entry
  `projectileInstanceBuffer_` (binding-1 translation for the projectile)
- `initCullingResources()` — one shared object bounding-sphere buffer
  (343 entries, not duplicated per LOD), 3 parallel sets of
  {visible-instance buffer, indirect draw buffer} — one set per LOD
  level — frustum uniform buffer, compute descriptor set (8 bindings),
  compute pipeline

### Projectile

Plain C++ class (no Vulkan includes), owned as a value member of
`VulkanContext`, mirroring `Camera`'s shape. `launch(origin, direction,
speed)` sets it flying; `update(deltaTime)` integrates position at
constant velocity and deactivates it after a fixed lifetime (~5s). No
collision detection against the instance grid yet — `position()` is the
one value a future milestone reads to add that. Rendered by reusing
LOD2's mesh + its own UBO/descriptor set/instance buffer (see
`TECHNICAL_NOTES.md` §17); drawn with a plain `vkCmdDrawIndexed` inside
`GeometryPass`, guarded by `isActive()` — no new `FrameGraph` pass.

### Lighting (PBR milestone 1)

Cook-Torrance BRDF (GGX distribution, Smith geometry, Fresnel-Schlick),
one directional light, no textures beyond the existing shared one. Two
pieces of data, two different mechanisms — see `TECHNICAL_NOTES.md` §19
for why:
- `SceneData` (light direction, light color+intensity, camera position)
  — a UBO at **binding 2** on the existing graphics descriptor set,
  shared by every material (both `descriptor_` and `projectileDescriptor_`
  reference the same `sceneDataBuffer_`), updated once per frame.
- `MaterialPushConstants` (albedo, metallic, roughness) — a fragment-
  stage **push constant**, re-issued via `vkCmdPushConstants` right
  before each draw call, since it's pipeline state that persists across
  draws in the same command buffer rather than per-draw-scoped like a
  descriptor set.

Tunable at runtime via an ImGui "Lighting" window (direction/color/
intensity sliders) — `FrameRenderer`'s `ImGuiPass` lambda mutates
`VulkanContext`'s light state directly.

### FrameRenderer

Owns per-frame synchronization (fence/semaphore pairs, double-buffered)
and the `FrameGraph` instance. `drawFrame()`:

1. Waits on the current frame's fence; reads back the *previous*
   frame's 3 LOD instance counts for the ImGui overlay (see
   `TECHNICAL_NOTES.md` §11 for why this happens here and not
   right after dispatch); acquires the next swapchain image
2. Records `executeCompute()` — runs outside any render pass; resets
   all 3 `IndirectDrawBuffer.instanceCount` to 0, uploads the current
   frame's frustum, dispatches the culling + LOD-select compute shader
3. Inserts a memory barrier (compute write → vertex/indirect read)
4. Begins the render pass, records `executeGraphics()` — binds each
   LOD's own vertex/index/visible-instance buffers and issues one
   `vkCmdDrawIndexedIndirect` per LOD (3 draw calls/frame) — ends the
   render pass
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

First-person controller: WASD for movement, mouse-look for yaw/pitch
(the window is set to `GLFW_CURSOR_DISABLED` once in `Application::init()`
so the cursor is hidden and reports an unbounded virtual position — see
`TECHNICAL_NOTES.md` §18 for why this replaced the original QE/arrow-key
scheme). Holding **Ctrl** reveals the cursor (`GLFW_CURSOR_NORMAL`) and
suspends mouse-look entirely, so the ImGui debug windows can actually be
clicked/dragged — GLFW's unbounded virtual cursor position in disabled
mode isn't real screen coordinates, so ImGui can't hit-test against it
otherwise (§18 addendum). Exposes `getViewMatrix()`, called once per
frame and shared identically by both the culling compute pass (frustum
construction) and the geometry pass (vertex transform) — see
`TECHNICAL_NOTES.md` §10 for why this single-source-of-truth matters.

### ObjLoader

Loads OBJ via tinyobjloader, deduplicates vertices into a `unordered_map`
keyed by position+normal+uv (Suzanne LOD0: 2904 → 507 unique vertices).
Called once per LOD mesh (`suzanne.obj` / `suzanne_lod1.obj` /
`suzanne_lod2.obj`), each returning its own `{ vertices, indices,
boundingRadius }`. Also recenters the mesh to its own bounding-box
center (raw OBJ coordinates aren't authored at local origin) and
computes `boundingRadius` — the max vertex distance from that new
center — used by `initCullingResources()` as the shared object bounding
sphere. See `TECHNICAL_NOTES.md` §16 for why recentering matters here
(per-instance rotation would otherwise orbit rather than spin in
place).

### Compute culling + LOD selection pipeline

- `ObjectBuffer` (SSBO, read-only in shader) — one shared array, 343
  entries, per-instance bounding sphere `(center.xyz, radius)`. Not
  duplicated per LOD — LOD level is a *selection* made per-frame, not a
  different object.
- `culling.comp` — 64 threads/workgroup:
  1. sphere-vs-6-plane frustum test (unchanged from the original
     single-LOD design)
  2. for threads that pass, a distance check against the camera
     (`LOD1_DIST = 12.0`, `LOD2_DIST = 20.0`, both hardcoded in-shader)
     buckets the instance into exactly one of 3 output sets
  3. the winning bucket's thread claims a slot via `atomicAdd` on that
     bucket's own `DrawCommand.instanceCount` and writes into that
     bucket's own `VisibleLODN` buffer
- 3 parallel output pairs — `(VisibleLOD0, IndirectLOD0)`,
  `(VisibleLOD1, IndirectLOD1)`, `(VisibleLOD2, IndirectLOD2)` at
  bindings 1–6, plus `ObjectBuffer` (0) and `FrustumData` (7) — 8
  bindings total in `ComputeDescriptor`
- Each `IndirectDrawBuffer.instanceCount` is reset to 0 by the CPU each
  frame before dispatch, exactly as in the single-LOD design — the
  reset now happens 3 times (once per LOD) instead of once
- `vkCmdDrawIndexedIndirect` — called once per LOD mesh (3 draw calls
  total per frame), each bound to that LOD's own vertex/index/visible-
  instance buffers; the CPU never reads back which instances passed
  culling, which LOD bucket they landed in, or how many during the
  frame that produced them — it only reads the resulting counts *after*
  the frame, for the debug overlay (§11)

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
    ├── VulkanDescriptor (graphics, 3 bindings: UBO / combined-image-sampler / SceneData)
    ├── ComputeDescriptor (compute, 8 bindings: object / 3×visible / 3×indirect / frustum)
    ├── lods_[3] : LODMesh { VertexBuffer, IndexBuffer, VisibleInstanceBuffer, IndirectDrawBuffer }
    ├── ObjectBuffer (shared, 343 entries) / FrustumBuffer
    ├── VulkanTexture
    ├── sceneDataBuffer_ (shared UBO: light direction/color/intensity, camera pos —
    │     bound at binding 2 on both descriptor_ and projectileDescriptor_)
    ├── Projectile (plain C++, position/direction/speed/lifetime)
    ├── projectileUniformBuffer_ / projectileDescriptor_ / projectileInstanceBuffer_
    │     (own UBO+descriptor+1-entry instance buffer — reuses lods_[2]'s mesh)
    └── Camera
        └── FrameRenderer
            └── FrameGraph
                ├── GPUCullingPass (Compute) — frustum test + LOD fan-out
                └── GeometryPass (Graphics) — per-draw push constant
                                              (MaterialPushConstants: albedo/metallic/roughness)
                                              + 3× vkCmdDrawIndexedIndirect
                                              + 1× vkCmdDrawIndexed (projectile, if active)
```

> **Known dead resource:** `VulkanContext::instanceBuffer_` (the
> original static, CPU-uploaded-once instance buffer from the
> pre-culling design) is still created and destroyed but is no longer
> bound anywhere in the draw path — every LOD's `visibleInstanceBuffer`
> is what actually gets bound at binding 1 now. Same category of
> leftover as `visibilityBuffer_` in §12; flagged for removal alongside
> the other dead-code cleanup pass.

---

## Frame pipeline (as implemented)

```
Wait Fence → Read back previous frame's 3 LOD instance counts (debug overlay)
        │
Acquire Swapchain Image
        │
Reset IndirectLOD0/1/2.instanceCount = 0   (3× CPU write)
        │
Compute: GPUCullingPass
   (sphere-frustum test, 343 threads, distance-based LOD bucketing,
    atomic compaction per bucket)
        │
Memory Barrier (SHADER_WRITE → VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ)
        │
Begin Render Pass
        │
Graphics: GeometryPass
   (for each LOD: bind that LOD's vertex/index/visible-instance buffers,
    vkCmdDrawIndexedIndirect — GPU-supplied instance count)
        │
End Render Pass
        │
Queue Submit → Present
```

