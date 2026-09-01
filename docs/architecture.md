# Architecture Overview

This document describes the high-level architecture of the renderer.

> Superseded the original Phase-0-era version of this file (Instance/
> Surface/Device/Swapchain only). See `TECHNICAL_NOTES.md` for the
> decision-by-decision rationale behind each piece below.

---

## Current Architecture

```
Application                     — input polling, deltaTime, world-sim tick
├── Camera::processInput()        WASD + mouse-look (Ctrl reveals cursor for UI)
├── Projectile::launch()/update() left-click fires, straight-line flight
├── VulkanContext::updateInstanceSimulation()
│                                  velocity+damping integration, projectile-
│                                  vs-grid blast, mutual instance pushout,
│                                  re-uploads objectBuffer_ every frame
├── VulkanContext::updateSpin()   accumulated angle, T pauses/resumes
└── VulkanContext
    ├── initCore()              instance → device → swapchain → depth →
    │                            renderpass → pipeline (+ push-constant
    │                            range) → framebuffer; sceneDataBuffer_ +
    │                            2 descriptor sets (grid, projectile)
    ├── initSceneData()         3× OBJ load (LOD0/1/2), vertex/index
    │                            buffer per LOD, 7×7×7 instance grid,
    │                            1-entry projectile instance buffer
    └── initCullingResources()  shared object buffer (343 bounding
                                 spheres, re-uploaded every frame — see
                                 "Grid collision + scatter" below), 3×
                                 {visible-instance buffer, indirect draw
                                 buffer} — one pair per LOD, frustum
                                 uniform buffer, compute descriptor set
                                 (8 bindings), compute pipeline
        └── FrameRenderer
            └── FrameGraph (DAG, Kahn's algorithm)
                ├── GPUCullingPass   [Compute stage] — frustum test +
                │                     distance-based LOD fan-out
                ├── ShadowPass       [Shadow stage, own render pass +
                │                     fixed-resolution framebuffer] —
                │                     depth-only draw of all instances
                │                     from the light's point of view
                ├── GeometryPass     [Graphics stage, depends on
                │                     CullingPass + ShadowPass] — per-draw
                │                     material push constant + 3 indirect
                │                     draws (grid LODs) + 1 direct draw
                │                     (projectile, if active), sampling
                │                     the shadow map for occlusion
                └── ImGuiPass        stats overlay + live lighting/shadow
                                      sliders + shadow map debug preview
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
- Polling **R** (reset grid formation) and **T** (pause/resume grid
  spin), both edge-detected the same way as the click trigger, and
  driving `VulkanContext::updateInstanceSimulation()`/`updateSpin()`
  every frame (world-simulation state, not rendering state — kept here
  rather than inside a `FrameRenderer` pass)

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
  descriptor sets, since both reference it at binding 2. Also creates the
  shadow map's image/view/sampler (`shadowMap_`, before both descriptor
  sets, which bind it at binding 3) and, after the main pipeline/
  framebuffer, its depth-only render pass, fixed-resolution framebuffer,
  and pipeline (`shadowRenderPass_`/`shadowFramebuffer_`/
  `shadowPipeline_`) — see "Shadow mapping" below
- `initSceneData()` — OBJ mesh loading + deduplication for 3 LOD meshes
  (`suzanne.obj`, `suzanne_lod1.obj`, `suzanne_lod2.obj`), vertex/index
  buffer upload per LOD, 7×7×7 instance grid generation, plus a 1-entry
  `projectileInstanceBuffer_` (binding-1 translation for the projectile)
- `initCullingResources()` — one shared object bounding-sphere buffer
  (343 entries, not duplicated per LOD), 3 parallel sets of
  {visible-instance buffer, indirect draw buffer} — one set per LOD
  level — frustum uniform buffer, compute descriptor set (8 bindings),
  compute pipeline. `cachedInstances_` (the grid's rest positions) is
  kept alive here rather than freed — see "Grid collision + scatter"
  below.

### Grid collision + scatter (Phase 7 milestone 2)

`objectBuffer_` — the same 343-entry bounding-sphere buffer
`culling.comp` already reads every dispatch for visibility/LOD — is now
re-uploaded every frame from CPU-simulated positions instead of once at
startup. Zero compute shader or descriptor changes: `culling.comp` has
no separate concept of a "static" position, so making the upload
per-frame is a purely CPU-side change (cheap thanks to `VulkanBuffer`'s
persistent mapping). See `TECHNICAL_NOTES.md` §20 for the full
rationale, the framerate-independent damping formula, and the discrete-
collision tradeoff.

- `VulkanContext::updateInstanceSimulation(deltaTime)` — called once per
  frame from `Application::mainLoop()` (not from a `FrameRenderer` pass
  lambda — this is world-simulation state, not rendering state, and
  keeps `deltaTime` where it already lives). Integrates
  `instanceVelocities_` into `instanceCurrentPositions_` with damping,
  checks the active projectile against every instance (cheap `O(343)`,
  using `collisionRadius_` — a gameplay-tunable value independent of
  `boundingSphereRadius_`, the render/culling radius `culling.comp`
  uses; they start equal but can diverge), and on the first touch
  applies a radial blast impulse (falloff by distance, `+=`'d so
  overlapping blasts compound) to every instance within a blast radius,
  then stops the projectile. Also resolves **mutual instance-vs-instance
  overlap** every frame (positional pushout, `O(n²)` unique pairs, single
  pass) — without it, scattered instances settling near each other or
  near still-resting neighbors visibly clip through one another, since
  the grid's rest spacing leaves only ~0.02 units of margin between
  adjacent instances to begin with. See `TECHNICAL_NOTES.md` §21.
- `VulkanContext::resetInstanceFormation()` — restores
  `instanceCurrentPositions_` from the permanent `cachedInstances_` rest
  formation and zeroes all velocities. Triggered by an edge-detected
  **R** keypress in `Application::mainLoop()`.
- `VulkanContext::spinAngle()`/`updateSpin()`/`toggleSpinPaused()` — the
  grid's shared rotation now advances an accumulated angle instead of
  reading `glfwGetTime()` directly, so a **T**-key pause/resume (added
  alongside this feature) freezes/resumes smoothly instead of snapping.

### Projectile

Plain C++ class (no Vulkan includes), owned as a value member of
`VulkanContext`, mirroring `Camera`'s shape. `launch(origin, direction,
speed)` sets it flying; `update(deltaTime)` integrates position at
constant velocity and deactivates it after a fixed lifetime (~5s), or
immediately via `stop()` on grid impact (see "Grid collision + scatter"
above). Rendered by reusing LOD2's mesh + its own UBO/descriptor
set/instance buffer (see `TECHNICAL_NOTES.md` §17); drawn with a plain
`vkCmdDrawIndexed` inside `GeometryPass`, guarded by `isActive()` — no
new `FrameGraph` pass.

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
intensity/shadow-bias sliders) — `FrameRenderer`'s `ImGuiPass` lambda
mutates `VulkanContext`'s light state directly.

### Shadow mapping

Classic shadow mapping for the single directional light — see
`TECHNICAL_NOTES.md` §22 for why this was chosen over an analytic
ray-sphere-occlusion alternative that would have reused `culling.comp`'s
existing bounding spheres.

- `VulkanShadowMap` — a fixed-resolution (2048×2048), sampled
  (`D32_SFLOAT`) depth image/view/sampler, independent of swapchain size
  since it's rendered from the light's point of view, not the camera's.
  Unlike `VulkanDepthBuffer` (the swapchain's own depth attachment, never
  sampled), this one is read by the main fragment shader.
- `VulkanRenderPass::createDepthOnly()` / `VulkanFramebuffer::
  createDepthOnly()` — depth-only variants (no color attachment, single
  fixed-size framebuffer rather than one per swapchain image) alongside
  the existing swapchain-oriented `create()` methods.
- `VulkanShadowPipeline` — a sibling to `VulkanPipeline`/
  `VulkanComputePipeline` (a new `FrameGraph` stage getting its own
  pipeline class is an established pattern here, from when compute
  culling was added). Vertex-only (no fragment stage, no color blend
  attachment), a `ShadowPushConstants` push constant (light view-
  projection + the same per-draw model rotation `triangle.vert` applies —
  a grid-spin desync bug without it, see `TECHNICAL_NOTES.md` §22).
- `FrameGraph::PassStage::Shadow` / `executeShadow()` — a third pass
  stage alongside `Compute`/`Graphics`, mirroring how `executeCompute()`
  already runs outside the main render pass with its own explicit
  wrapper in `FrameRenderer::drawFrame()` rather than trying to make one
  `RGPass` carry per-pass render-pass/framebuffer state.
- `VulkanContext::lightViewProj()` — the light's orthographic
  view-projection, single-sourced the same way `Camera::
  getProjectionMatrix()` is: eye placed opposite the light direction at a
  fixed `kSceneRadius`-derived distance, looking at the origin, with a
  degenerate-`lookAt` guard (falls back to a different up-axis when the
  light points nearly straight down). Uses `glm::orthoRH_ZO()`, not
  `glm::ortho()` — see `TECHNICAL_NOTES.md` §22 for why the default GLM
  depth convention is silently wrong specifically for an orthographic
  matrix in this codebase (it's not `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so
  `Camera`'s own perspective matrix has the same underlying mismatch, but
  gets away with it).
- `ShadowPass` (`FrameRenderer.cpp`) — draws all `OBJECT_COUNT` grid
  instances unculled (no light-frustum culling at this instance count)
  plus the projectile if active, binding `objectBuffer_` directly as the
  per-instance vertex input: it already holds `vec4(position, radius)`
  per instance, re-uploaded every frame by `updateInstanceSimulation()`,
  byte-identical to `InstanceData`'s layout — no new buffer needed.
- `SceneData` gained `lightViewProj` (read by `triangle.vert`, which now
  also binds binding 2, previously fragment-only) and `shadowParams.x`
  (base shadow bias, ImGui-tunable). `triangle.frag` samples the shadow
  map (binding 3 on the graphics descriptor set) with a slope-scaled bias
  and 3×3 PCF, multiplying only the direct `Lo` term — the flat ambient
  term stays unshadowed.

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
   (reading whatever `Application::mainLoop()` already wrote into
   `objectBuffer_` this frame via `updateInstanceSimulation()`)
3. Inserts a memory barrier (compute write → vertex/indirect read)
4. Begins the render pass, records `executeGraphics()`: uploads
   `SceneData` (light + camera), pushes the grid's material constants,
   binds each LOD's own vertex/index/visible-instance buffers and issues
   one `vkCmdDrawIndexedIndirect` per LOD (3 draw calls/frame); if the
   projectile is active, pushes its own (distinct) material constants
   and issues one `vkCmdDrawIndexed`; runs `ImGuiPass` last — ends the
   render pass
5. Submits, presents

Note: the input polling, `Projectile::update()`, `updateInstanceSimulation()`
(grid collision/scatter/mutual-collision), and `updateSpin()` steps all
happen in `Application::mainLoop()` **before** `drawFrame()` is called —
see "Grid collision + scatter" above for why that world-simulation state
lives there rather than inside a `FrameRenderer` pass.

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
    ├── VulkanShadowMap / VulkanRenderPass (depth-only) / VulkanFramebuffer
    │     (depth-only) / VulkanShadowPipeline (see "Shadow mapping" module notes)
    ├── VulkanDescriptor (graphics, 4 bindings: UBO / combined-image-sampler /
    │     SceneData / shadow-map combined-image-sampler)
    ├── ComputeDescriptor (compute, 8 bindings: object / 3×visible / 3×indirect / frustum)
    ├── lods_[3] : LODMesh { VertexBuffer, IndexBuffer, VisibleInstanceBuffer, IndirectDrawBuffer }
    ├── ObjectBuffer (shared, 343 entries, re-uploaded every frame) / FrustumBuffer
    ├── cachedInstances_ / instanceCurrentPositions_ / instanceVelocities_
    │     (rest formation / live scatter state, all 343 entries — see
    │      "Grid collision + scatter" module notes)
    ├── VulkanTexture
    ├── sceneDataBuffer_ (shared UBO: light direction/color/intensity, camera pos,
    │     lightViewProj, shadow bias — bound at binding 2 on both descriptor_ and
    │     projectileDescriptor_, now also read by the vertex stage)
    ├── Projectile (plain C++, position/direction/speed/lifetime)
    ├── projectileUniformBuffer_ / projectileDescriptor_ / projectileInstanceBuffer_
    │     (own UBO+descriptor+1-entry instance buffer — reuses lods_[2]'s mesh)
    └── Camera
        └── FrameRenderer
            └── FrameGraph
                ├── GPUCullingPass (Compute) — frustum test + LOD fan-out
                ├── ShadowPass (Shadow) — depth-only, all instances unculled,
                │                          objectBuffer_ reused as instance buffer
                └── GeometryPass (Graphics, reads CullingPass + ShadowPass) —
                                              per-draw push constant
                                              (MaterialPushConstants: albedo/metallic/roughness)
                                              + 3× vkCmdDrawIndexedIndirect
                                              + 1× vkCmdDrawIndexed (projectile, if active)
```

> **Resolved:** `VulkanContext::instanceBuffer_` (the original static,
> CPU-uploaded-once instance buffer from the pre-culling design) used to
> be dead weight here — created/destroyed but never bound anywhere,
> since every LOD's `visibleInstanceBuffer` is what actually gets bound
> at binding 1. It and the now-orphaned `InstanceBuffer` class have
> since been deleted entirely (same cleanup pass that removed
> `RGPass::pipeline`). Kept as a historical note, same category as the
> `visibilityBuffer_` find in §12.

---

## Frame pipeline (as implemented)

```
Application::mainLoop()  (before FrameRenderer::drawFrame() is even called)
        │
Poll input: WASD/mouse-look, click (fire projectile), R (reset grid),
            T (toggle spin), Ctrl (reveal cursor for ImGui), Esc (quit)
        │
Projectile::update(dt)              — flight, lifetime expiry
        │
VulkanContext::updateInstanceSimulation(dt)
   ├─ integrate velocity + damping into instanceCurrentPositions_
   ├─ projectile-vs-grid blast check (one explosion per flight)
   ├─ mutual instance-vs-instance overlap pushout (O(n²), every frame)
   └─ re-upload objectBuffer_ from the updated positions
        │
VulkanContext::updateSpin(dt)       — accumulated angle, skipped if paused
        │
════════ FrameRenderer::drawFrame() ════════
        │
Wait Fence → Read back previous frame's 3 LOD instance counts (debug overlay)
        │
Acquire Swapchain Image
        │
Reset IndirectLOD0/1/2.instanceCount = 0   (3× CPU write)
        │
Compute: GPUCullingPass
   (sphere-frustum test using this frame's objectBuffer_, 343 threads,
    distance-based LOD bucketing, atomic compaction per bucket)
        │
Memory Barrier (SHADER_WRITE → VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ)
        │
Begin Shadow Render Pass (own framebuffer, fixed 2048×2048 resolution)
        │
Shadow: ShadowPass
   push {lightViewProj, spin rotation} → bind LOD0 vertex/index buffers +
   objectBuffer_ (as instance buffer) → vkCmdDrawIndexed, instanceCount =
   OBJECT_COUNT → if projectile active: push {lightViewProj, identity},
   bind LOD2 mesh + its instance buffer, vkCmdDrawIndexed
        │
End Shadow Render Pass
        │
Image Memory Barrier (LATE_FRAGMENT_TESTS write → FRAGMENT_SHADER read)
        │
Begin Render Pass
        │
Graphics: GeometryPass
   upload SceneData (light + camera + lightViewProj + shadow bias) →
   push grid material constants → for each LOD: bind vertex/index/
   visible-instance buffers, vkCmdDrawIndexedIndirect (GPU-supplied
   instance count, fragment shader samples the shadow map for occlusion) →
   if projectile active: push its own material constants,
   bind LOD2 mesh + its instance buffer, vkCmdDrawIndexed
        │
Graphics: ImGuiPass — stats overlay + live lighting/shadow sliders +
   shadow map debug preview
        │
End Render Pass
        │
Queue Submit → Present
```

