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
                ├── GPUCullingPass   [Compute stage] — coarse per-cluster
                │                     frustum test, then fine per-object
                │                     frustum test + distance-based LOD
                │                     fan-out (see "Hierarchical /
                │                     two-stage GPU culling" below)
                ├── ShadowPass       [Shadow stage, own render pass +
                │                     fixed-resolution framebuffer] —
                │                     depth-only draw of all instances
                │                     from the light's point of view
                ├── GeometryPass     [Graphics stage, depends on
                │                     CullingPass + ShadowPass, renders
                │                     into the offscreen sceneFramebuffer_,
                │                     NOT the swapchain] — per-draw
                │                     material push constant + 3 indirect
                │                     draws (grid LODs) + 1 direct draw
                │                     (projectile, if active), sampling
                │                     the shadow map for occlusion
                └── ImGuiPass        [UI stage, own render pass = the
                                      swapchain's] — dockable "Viewport"
                                      window (samples GeometryPass's output)
                                      + stats overlay + live lighting/shadow
                                      sliders + shadow map debug preview,
                                      all docked beside the Viewport rather
                                      than overlapping it (see "Dockable
                                      viewport" below)
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
- Driving `Camera::processInput()` each frame, then immediately syncing
  `ImGuiConfigFlags_NoMouse` with `Camera::cursorVisible()` every frame —
  see `TECHNICAL_NOTES.md` §27 for why this has to happen every frame,
  not just on the Ctrl transition
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
  overlap** every frame (`O(n²)` unique pairs, single pass) — without it,
  scattered instances settling near each other or near still-resting
  neighbors visibly clip through one another, since the grid's rest
  spacing leaves only ~0.02 units of margin between adjacent instances to
  begin with. A hybrid response as of §30: an equal-mass, restitution-
  scaled velocity impulse along the contact normal (only applied while a
  pair is actually approaching) provides the visible bounce/deflection,
  paired with a lighter positional-correction term (down from the
  original pure-pushout's 30%/side to 10%/side) that only mops up
  resting overlap an impulse alone can't resolve. `restitution()`/
  `setRestitution()` (default `0.3f`) is runtime-tunable via the "GPU
  Culling Stats" ImGui window's "Collision" section, same reasoning as
  `lod1ScreenSize()`/`lod2ScreenSize()`. See `TECHNICAL_NOTES.md` §21/§30.
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

### Lighting (PBR, both milestones)

Cook-Torrance BRDF (GGX distribution, Smith geometry, Fresnel-Schlick),
one directional light. Two pieces of data, two different mechanisms —
see `TECHNICAL_NOTES.md` §19 for why:
- `SceneData` (light direction, light color+intensity, camera position)
  — a UBO at **binding 2** on the existing graphics descriptor set,
  shared by every material (both `descriptor_` and `projectileDescriptor_`
  reference the same `sceneDataBuffer_`), updated once per frame.
- `MaterialPushConstants` (albedo, metallic, roughness) — a fragment-
  stage **push constant**, re-issued via `vkCmdPushConstants` right
  before each draw call, since it's pipeline state that persists across
  draws in the same command buffer rather than per-draw-scoped like a
  descriptor set. Milestone 2 kept this mechanism as a *factor*
  multiplying the new textures below, rather than replacing it, so grid
  vs. projectile still look visually distinct sharing one `Material`.

Milestone 2 (see `TECHNICAL_NOTES.md` §25) added real textures behind
those flat values:
- `Material` (`include/vulkan/texture/Material.h`) — bundles 4
  `VulkanTexture` instances (albedo/normal/metallic-roughness/AO).
  `VulkanContext::material_` is the one shared instance both
  `descriptor_` and `projectileDescriptor_` bind, same "reuses the
  single shared texture" pattern milestone 1 already used for the lone
  albedo texture.
  `VulkanTexture::create()` gained a `VkFormat` parameter: albedo stays
  `VK_FORMAT_R8G8B8A8_SRGB` (color data, sRGB→linear on sample);
  normal/metallic-roughness/AO use `VK_FORMAT_R8G8B8A8_UNORM` (non-color
  data must not be gamma-decoded).
- `VulkanDescriptor` grew from 4 to 7 bindings (4 = normal, 5 =
  metallic-roughness, 6 = AO, all fragment-stage combined-image-samplers).
- `triangle.frag`: metallic/roughness sample a glTF-convention
  metallic-roughness map (G = roughness, B = metallic), multiplied by the
  existing push-constant factors. AO multiplies the ambient term only
  (the direct term already has its own occlusion source, the shadow map
  — same "don't double up two different occlusion signals" discipline as
  `calcShadow()` touching only `Lo`). Normal mapping reconstructs a
  per-pixel tangent frame from `dFdx`/`dFdy` on world position and UV
  (Schuler's derivative-based technique) instead of a precomputed tangent
  vertex attribute — no `Vertex`/`ObjLoader` changes needed, at the cost
  of needing a guard for meshes with no real UV variation (see §25's NaN
  bug writeup).
- LOD0 loads a new asset, `assets/suzanne_pbr.obj` — a UV/normal-mapped
  re-export of the same base Suzanne mesh, since the original had zero
  UV data. LOD1/LOD2 are unchanged (still UV-less, sample a constant
  texel) — a known, explicitly accepted gap, not a regression.

Tunable at runtime via an ImGui "Lighting" window (direction/color/
intensity/shadow-bias sliders) — `FrameRenderer`'s `ImGuiPass` lambda
mutates `VulkanContext`'s light state directly.

### Image-Based Lighting (Milestones 1-3: cubemap infra + procedural sky + diffuse irradiance + specular prefilter/BRDF LUT)

Closes the roadmap's "IBL / environment lighting" gap entirely — staged
across multiple milestones (see `TECHNICAL_NOTES.md` §33/§34/§35).

- `VulkanCubemap` (`include/vulkan/texture/VulkanCubemap.h`) — this
  project's first cube image: one `VK_IMAGE_VIEW_TYPE_CUBE` view for
  sampling (`samplerCube`) plus, per mip level, 6 `VK_IMAGE_VIEW_TYPE_2D`
  single-layer views for use as render-pass color attachments (Vulkan
  render passes attach 2D views per subresource, not cube views).
  `mipLevels` defaults to 1 (still what `environmentCubemap_`/
  `irradianceCubemap_` use) but is a real parameter as of Milestone 3,
  which extended it for `prefilteredCubemap_`'s 5-mip chain.
- `VulkanContext::initEnvironment()` — called right after `initCore()`,
  before `initSceneData()`/`initCullingResources()`. Creates the
  persistent `environmentCubemap_` (512×512/face,
  `VK_FORMAT_R16G16B16A16_SFLOAT` — HDR-capable, the sun highlight is
  deliberately >1.0), then does a 6-draw one-shot bake into it using
  locally-scoped (not `VulkanContext`-owned) render pass/framebuffer/
  pipeline resources, reusing the exact one-shot command buffer pattern
  `VulkanTexture`'s setup-time layout transitions already established.
- `VulkanRenderPass::createColorOnly()` / `VulkanFramebuffer::
  createColorOnly()` — new no-depth variants alongside the existing
  `create()`/`createDepthOnly()`/`createOffscreenColor()` methods, for
  the bake's pure per-pixel-function render pass.
- `shaders/fullscreenTriangle.vert` — a `gl_VertexIndex`-driven
  fullscreen triangle, no vertex buffer, no uniform input. Shared
  (compiled once) by both the bake pipeline
  (`VulkanEnvCapturePipeline` + `shaders/envCapture.frag`, the
  procedural sky function) and the live skybox pipeline
  (`VulkanSkyboxPipeline` + `shaders/skybox.frag`, samples the baked
  cubemap) — a deliberate reuse, since nothing in it is pipeline-specific.
- Direction reconstruction (both the bake capture and the live skybox)
  uses `inverse(viewProj) * ndc`, not hand-derived per-face basis
  vectors — self-consistent by construction, see `TECHNICAL_NOTES.md`
  §33 for why this replaced an earlier basis-vector design.
- The live skybox draw is recorded inside `GeometryPass`'s existing
  lambda (`FrameRenderer.cpp`), first, before the grid's pipeline bind —
  no new `FrameGraph` pass. `depthTestEnable`/`depthWriteEnable` both
  `false`, so the grid's own depth test afterward overdraws it wherever
  geometry exists, with no z-value trickery needed.

**Milestone 2 (diffuse irradiance convolution):**

- `irradianceCubemap_` (32×32/face — diffuse irradiance is extremely
  low-frequency after cosine-weighted convolution, no need for
  `environmentCubemap_`'s 512×512) baked by
  `VulkanIrradiancePipeline` + `shaders/irradianceConvolve.frag` (the
  standard cosine-weighted hemisphere Riemann-sum integral), reusing the
  `inverse(viewProj)` direction-reconstruction technique and the same
  6-face capture table Milestone 1 already established.
- All bakes share **one command buffer, one submit** (extended further
  by Milestone 3 below): 6 environment-capture draws → a memory barrier
  → 6 irradiance-convolution draws (sampling the just-baked
  `environmentCubemap_` through `skyboxDescriptor_`) → a second barrier
  → 30 specular-prefilter draws (5 mips × 6 faces) → a barrier → 1 BRDF
  LUT draw → a barrier → submit. `initEnvironment()`'s call site moved
  from after `initCore()` to inside it (right after
  `sceneFramebuffer_.create()`, before `pipeline_.create()`) since
  `pipeline_.create()` needs `iblDescriptor_`'s layout.
- `SkyboxDescriptor` renamed to `CubeSamplerDescriptor` (its shape was
  always generic — one combined-image-sampler binding — reused for the
  live skybox draw, the irradiance-bake input, and the prefilter-bake
  input, all bound to `environmentCubemap_` via the same
  `skyboxDescriptor_` instance where applicable).
- `VulkanPipeline` grew a **second descriptor set** — this codebase's
  first multi-set pipeline layout. Set 0 = `descriptor_`/
  `projectileDescriptor_`'s existing material data; set 1 =
  `iblDescriptor_`, ambient-lighting data shared by every material
  (scene-wide, not per-object), bound once per frame in `GeometryPass`
  and left bound across the grid→projectile set-0 switch (Vulkan's
  descriptor-set binding-persistence rule). `VulkanDescriptor` itself
  needed zero changes.

**Milestone 3 (specular prefilter + BRDF LUT — Karis's split-sum
approximation, completing the ambient term):**

- `prefilteredCubemap_` (5 mips, 128×128 down to 8×8, roughness
  0.0/0.25/0.5/0.75/1.0) baked by 5 short-lived `VulkanPrefilterPipeline`
  instances (one per mip — this codebase has no dynamic-viewport-state
  precedent, every pipeline class bakes a static viewport) +
  `shaders/prefilterEnv.frag` (GGX-importance-sampled, `N=V=R`
  simplifying assumption).
- `VulkanBRDFLut` (`include/vulkan/texture/VulkanBRDFLut.h`, new) — a
  single 2D image+view+sampler (`R16G16_SFLOAT`, 512×512), baked by
  `VulkanBRDFLutPipeline` + `shaders/brdfLUT.frag`: a pure function of
  `(NdotV, roughness)`, needing no descriptor set or push constant at
  all (unlike every other bake here). Uses its own IBL-specific
  Schlick-GGX `k = roughness²/2`, deliberately different from
  `triangle.frag`'s direct-light `k = (roughness+1)²/8`.
- `IBLDescriptor` (`include/vulkan/descriptor/IBLDescriptor.h`, new, 3
  bindings: irradiance cubemap / prefiltered cubemap / BRDF LUT)
  replaces the single-binding `irradianceDescriptor_` as `pipeline_`'s
  set 1 entirely.
- `VulkanCubemap` fixed a real, previously-silent sampler bug in the
  process of gaining mip support: `minLod`/`maxLod` were never set
  (zero-initialized to 0), which clamps every sampled LOD to mip 0
  regardless of what a shader requests — harmless at 1 mip, would have
  been a real bug the moment `prefilteredCubemap_`'s 5 mips were sampled.
- `triangle.frag` gained `prefilteredMap`/`brdfLUT` (set 1, bindings 1-2)
  and a `specularIBL` term; upgraded `kS_ambient` from plain
  `fresnelSchlick` to the roughness-aware `fresnelSchlickRoughness`
  (used by both diffuse and specular ambient); `ambient =
  (diffuseIBL + specularIBL) * ao`.

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
  distance derived from a `sceneRadius` computed fresh every frame from
  `instanceCurrentPositions_` (max distance from the origin, plus
  `boundingSphereRadius_` margin, floored at `kMinSceneRadius = 24.0f` —
  the grid's rest-formation coverage) rather than the fixed constant it
  used to be, so a projectile blast (`updateInstanceSimulation()`'s
  scatter) that pushes instances outward grows the light's frustum to
  match instead of silently clipping them out of the shadow map — see
  `TECHNICAL_NOTES.md` §29. Looks at the origin, with a
  degenerate-`lookAt` guard (falls back to a different up-axis when the
  light points nearly straight down). Uses `glm::orthoRH_ZO()`, not
  `glm::ortho()` — see `TECHNICAL_NOTES.md` §22 for why the default GLM
  depth convention is silently wrong specifically for an orthographic
  matrix in this codebase (it's not `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so
  `Camera`'s own perspective matrix has the same underlying mismatch, but
  gets away with it).
- `ShadowPass` (`FrameRenderer.cpp`) — draws the grid instances that
  survive light-frustum culling (see "Light-frustum culling for the
  shadow pass" below) via `vkCmdDrawIndexedIndirect`, plus the
  projectile directly (unculled) if active.
- `SceneData` gained `lightViewProj` (read by `triangle.vert`, which now
  also binds binding 2, previously fragment-only) and `shadowParams.x`
  (base shadow bias, ImGui-tunable). `triangle.frag` samples the shadow
  map (binding 3 on the graphics descriptor set) with a slope-scaled bias
  and 3×3 PCF, multiplying only the direct `Lo` term — the flat ambient
  term stays unshadowed.

### Light-frustum culling for the shadow pass

`ShadowPass` used to draw all `OBJECT_COUNT` instances unculled every
frame. It now reuses the same `culling.comp` dispatch `GPUCullingPass`
already runs for the camera path — see `TECHNICAL_NOTES.md` §28 for the
full rationale, including why the camera path's plane-extraction code
was re-verified as correct *before* reusing it for the light, and why a
naive reuse would have introduced a silent bug.

- `culling.comp` runs an independent 6-plane sphere test against the
  light's orthographic frustum for every object, unconditional on
  camera visibility (a shadow caster can be off-screen), and compacts
  survivors into a 4th `{ShadowVisible, ShadowIndirect}` output pair —
  same atomic-counter compaction shape as the 3 camera-visible LOD
  buckets.
- `FrustumPlanes::extractFromMatrix` (`include/vulkan/culling/
  Frustum.h`) gained a `zeroToOne` parameter (default `false`, camera
  call site unchanged) selecting the near-plane formula matching the
  source matrix's depth convention: `m[3]+m[2]` for GLM's default
  `[-1,1]` (the camera's `glm::perspective()`), `m[2]` alone for
  Vulkan's native `[0,1]` (the light's `glm::orthoRH_ZO()`, see "Shadow
  mapping" above). `GPUCullingPass` calls it with `zeroToOne=true` for
  the light frustum.
- `ComputeDescriptor` grew from 8 to 11 bindings (8: shadow-visible
  instance buffer, 9: shadow indirect-draw buffer, 10: light-frustum
  UBO — reuses the `FrustumPlanes` struct/layout, only `planes[6]` is
  meaningful for the light).
- `VulkanContext::shadowVisibleInstanceBuffer()` /
  `shadowIndirectDrawBuffer()` / `lightFrustumBuffer()` — new buffers,
  same `{visibleInstanceBuffer, indirectDrawBuffer}` shape a single
  LOD's already uses, sized for worst case (all `OBJECT_COUNT` visible),
  indexed by LOD0's index count (the shadow pass always draws LOD0
  geometry regardless of camera-side LOD selection).
- `ShadowPass` binds `shadowVisibleInstanceBuffer()` as its per-instance
  vertex input and issues `vkCmdDrawIndexedIndirect` against
  `shadowIndirectDrawBuffer()`, replacing the old direct
  `vkCmdDrawIndexed(..., OBJECT_COUNT, ...)` against `objectBuffer_`.
  The existing compute→graphics `VkMemoryBarrier` in
  `FrameRenderer::drawFrame()` already covers these new buffers (it's a
  blanket `SHADER_WRITE → VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ`,
  not scoped to specific buffers) — no new barrier needed.

### Dockable viewport (Phase 11)

The 3 debug windows (`GPU Culling Stats` / `Lighting` / `Shadow Map`) used
to be plain `ImGui::Begin()` windows with no assigned position, cascading
on top of the rendered grid. See `TECHNICAL_NOTES.md` §24 for the full
tradeoff analysis (why not just reposition the windows, why not a custom
static `VkViewport` split, why not Qt) behind the approach below.

- `VulkanSceneColorTarget` — originally a fixed 1280×720, sampled
  (`VK_FORMAT_B8G8R8A8_SRGB`) color image/view, independent of swapchain
  size — same "sampled render target, own render pass" shape as
  `VulkanShadowMap`, just a color attachment instead of depth. Paired
  with its own `VulkanDepthBuffer` instance (`sceneColorDepth_`),
  separate from the swapchain's. Live-resizable as of Phase 16 (see
  below) — `create()` now takes `width`/`height` as runtime parameters.
- `VulkanRenderPass::createOffscreenColor()` — a third render-pass
  variant alongside `create()`/`createDepthOnly()`: color + depth
  attachments like `create()`, but the color attachment's final layout is
  `SHADER_READ_ONLY_OPTIMAL` (sampled by ImGui afterwards) instead of
  `PRESENT_SRC_KHR`.
- `FrameGraph::PassStage` gained a 4th value, `UI`, alongside
  `Compute`/`Shadow`/`Graphics` — same "a genuinely new render pass needs
  a new stage" extension this codebase already used once for `Shadow`
  (`docs/setup.md` §8 documents this as the sanctioned pattern).
  `GeometryPass`/`LightingPass`/`PostProcess` stay `Graphics`, now
  understood as "the offscreen scene pass"; `ImGuiPass` moved to `UI`,
  the swapchain's own render pass — which now hosts *only* UI, no 3D
  geometry.
- `VulkanContext::pipeline_` (the geometry pipeline) targets the new
  `sceneRenderPass_`/`sceneColorTarget_.extent()` instead of the
  swapchain's `renderPass_`/extent — same shaders/descriptor layout, both
  extents happen to already be `1280×720`.
- `ImGuiPass` calls `ImGui::DockSpaceOverViewport()` plus a one-time
  `DockBuilder*` layout (Viewport centered, the 3 debug windows docked as
  a tabbed group on the right) so the first run already shows the
  intended layout. The scene color target is registered with
  `ImGui_ImplVulkan_AddTexture()` and displayed in a new "Viewport"
  window — the exact mechanism the Shadow Map debug preview already used
  (see "Shadow mapping" above), aimed at the main color output instead.
- `third_party/imgui` repointed from a non-docking commit to the
  `docking` branch (`ImGuiConfigFlags_DockingEnable` didn't exist
  before). No new source files.
- Originally fixed-resolution, not resized to the panel's pixel size —
  `ImGui::Image()` just scaled the existing texture to whatever size the
  panel ended up being. Live-resized as of Phase 16, see below.

### Live-resized viewport target (Phase 16)

Closes the fixed-resolution limitation the section above originally
shipped with — see `TECHNICAL_NOTES.md` §36 for the full design
(why `vkDeviceWaitIdle` and not a per-slot fence wait, why the resize is
detected in `ImGuiPass` but applied at the top of the *next* frame, why
no debounce).

- `VulkanContext::resizeSceneTarget(width, height)` — destroys and
  recreates `sceneFramebuffer_`/`pipeline_`/`skyboxPipeline_`/
  `sceneColorDepth_`/`sceneColorTarget_` at a new size (`pipeline_`/
  `skyboxPipeline_` need it since both bake a static `VkViewport` at
  creation time, this codebase's universal pipeline pattern — see §35's
  same finding for the specular prefilter's 5 pipeline instances).
  `sceneRenderPass_` is untouched (format/structure only, no extent).
  Clamps to a 64px floor itself; no-ops if the size already matches.
- `Camera::getProjectionMatrix(aspectRatio)` — `Camera::ASPECT_RATIO`
  (a `1280/720` compile-time constant) is gone; every call site
  (`GPUCullingPass`'s frustum construction, `GeometryPass`'s grid and
  projectile UBOs) recomputes the aspect ratio fresh each frame from
  `sceneColorTarget().extent()`. The screen-space LOD threshold's
  projection scale (`Tunable LOD thresholds` above) picked up the same
  fix, since it also read the target's height.
- `FrameRenderer` detects a size mismatch in `ImGuiPass` (comparing
  `ImGui::GetContentRegionAvail()` against the current extent) and
  queues it (`resizePending_`/`pendingWidth_`/`pendingHeight_`);
  `drawFrame()` applies it at the very top of the next call, before any
  per-frame-slot state is touched, then re-registers `sceneViewportSet_`
  (`ImGui_ImplVulkan_RemoveTexture`/`AddTexture`) against the new
  `VkImageView`.

### FrameRenderer

Owns per-frame synchronization (fence/semaphore pairs, double-buffered)
and the `FrameGraph` instance. `drawFrame()`:

1. Waits on the current frame's fence; reads back the *previous*
   frame's 3 LOD instance counts for the ImGui overlay (see
   `TECHNICAL_NOTES.md` §11 for why this happens here and not
   right after dispatch) and, if the GPU supports timestamp queries, that
   same frame slot's 4 GPU timestamps (converted to Culling/Shadow/
   Graphics/Total ms) — same safe-readback timing as the LOD counts,
   since the fence wait already guarantees the slot's prior GPU work is
   done (see `TECHNICAL_NOTES.md` §23); acquires the next swapchain image
2. Resets this frame's timestamp query pool and writes timestamp 0
   (`TOP_OF_PIPE`), then records `executeCompute()` — runs outside any
   render pass; resets all 3 `IndirectDrawBuffer.instanceCount` to 0
   plus the shadow pass's own `shadowIndirectDrawBuffer_.instanceCount`,
   uploads the current frame's camera frustum *and* light frustum (see
   "Light-frustum culling for the shadow pass" above), dispatches the
   culling + LOD-select compute shader (reading whatever
   `Application::mainLoop()` already wrote into `objectBuffer_` this
   frame via `updateInstanceSimulation()`) — which also compacts
   light-frustum survivors into `shadowVisibleInstanceBuffer_`/
   `shadowIndirectDrawBuffer_`; writes timestamp 1 (`BOTTOM_OF_PIPE`)
3. Inserts a memory barrier (compute write → vertex/indirect read) —
   covers both the camera-side LOD buffers and the shadow pass's new
   ones, since it's a blanket `VkMemoryBarrier` not scoped to specific
   buffers
4. Begins the shadow render pass, records `executeShadow()` (see "Shadow
   mapping" above — now a `vkCmdDrawIndexedIndirect` against the
   light-frustum-culled instance set, not an unculled direct draw), ends
   it, writes timestamp 2; inserts the shadow→main image barrier (depth
   write → fragment-shader read)
5. Begins the **offscreen scene render pass** (`sceneRenderPass_`/
   `sceneFramebuffer_`, see "Dockable viewport" above), records
   `executeGraphics()`: uploads `SceneData` (light + camera + shadow
   data), pushes the grid's material constants, binds each LOD's own
   vertex/index/visible-instance buffers and issues one
   `vkCmdDrawIndexedIndirect` per LOD (3 draw calls/frame); if the
   projectile is active, pushes its own (distinct) material constants and
   issues one `vkCmdDrawIndexed` — ends the render pass; inserts a color
   barrier (color write → fragment-shader read) so ImGui can safely
   sample the result
6. Begins the **swapchain's render pass** (UI only), records
   `executeUI()` (`ImGuiPass` — dockable Viewport + debug windows) — ends
   the render pass, writes timestamp 3
7. Submits, presents

Note: the input polling, `Projectile::update()`, `updateInstanceSimulation()`
(grid collision/scatter/mutual-collision), and `updateSpin()` steps all
happen in `Application::mainLoop()` **before** `drawFrame()` is called —
see "Grid collision + scatter" above for why that world-simulation state
lives there rather than inside a `FrameRenderer` pass.

### FrameGraph

A DAG of render passes. Each `RGPass` declares:
- `name`
- `reads` — indices of passes this one depends on
- `stage` — `Compute`, `Shadow`, `Graphics`, or `UI`
- `execute` — the actual command-recording lambda

`build()` resolves execution order via Kahn's algorithm (BFS topological
sort using indegree counts), throwing on cycle detection. `executeCompute()`
/ `executeShadow()` / `executeGraphics()` / `executeUI()` walk the resolved
order, filtering by stage.

### Camera

First-person controller: WASD for movement, mouse-look for yaw/pitch
(the window is set to `GLFW_CURSOR_DISABLED` once in `Application::init()`
so the cursor is hidden and reports an unbounded virtual position — see
`TECHNICAL_NOTES.md` §18 for why this replaced the original QE/arrow-key
scheme). Holding **Ctrl** reveals the cursor (`GLFW_CURSOR_NORMAL`) and
suspends mouse-look entirely, so the ImGui debug windows can actually be
clicked/dragged — GLFW's unbounded virtual cursor position in disabled
mode isn't real screen coordinates, so ImGui can't hit-test against it
otherwise (§18 addendum). `cursorVisible()` exposes this state publicly
so `Application::mainLoop()` can keep ImGui's own mouse capture in sync
with it — see "Application" above and `TECHNICAL_NOTES.md` §27 for the
inverse problem this solves (ImGui still processing the unbounded
disabled-cursor position as if it were real, and incorrectly believing
it has mouse capture). Exposes `getViewMatrix()`, called once per
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
- `culling.comp` (the **fine** pass — see "Hierarchical / two-stage GPU
  culling" below for the coarse pass that now runs before it) — 64
  threads/workgroup:
  1. recovers this object's cluster index from its linear grid index
     (must match `VulkanContext::clusterIndexForInstance()` and
     `initSceneData()`'s generation order — see below), then reads that
     cluster's two coarse-pass visibility flags
  2. sphere-vs-6-plane test against the light's frustum, unconditional
     on camera visibility (a shadow caster can be off-screen) but now
     skipped entirely if the whole cluster already failed the coarse
     light test — passing threads claim a slot in the shared
     `ShadowVisible`/`ShadowIndirect` output pair (see "Light-frustum
     culling for the shadow pass" above)
  3. sphere-vs-6-plane frustum test against the camera, likewise skipped
     (early `return`) if the whole cluster already failed the coarse
     camera test — otherwise unchanged from the original single-LOD
     design
  4. for threads that pass the camera test, a screen-space projected-size
     check (`frustum.lodParams.x`/`.y`, runtime-tunable — see "Tunable
     LOD thresholds" below) buckets the instance into exactly one of 3
     output sets
  5. the winning bucket's thread claims a slot via `atomicAdd` on that
     bucket's own `DrawCommand.instanceCount` and writes into that
     bucket's own `VisibleLODN` buffer
- 3 parallel camera-side output pairs — `(VisibleLOD0, IndirectLOD0)`,
  `(VisibleLOD1, IndirectLOD1)`, `(VisibleLOD2, IndirectLOD2)` at
  bindings 1–6 — plus `ObjectBuffer` (0), `FrustumData` (7, camera
  frustum), `ShadowVisible`/`ShadowIndirect` (8–9), `LightFrustumData`
  (10), and the hierarchical-culling bindings (11–13, see below) — 14
  bindings total in `ComputeDescriptor`

### Hierarchical / two-stage GPU culling

Closes a previously-open roadmap item ("Multi-pass / hierarchical
culling") — see `TECHNICAL_NOTES.md` §31 for the full rationale
including a correctness proof that this can never change the final
visible set, honest at-this-scale performance framing, and the accepted
static-clustering limitation.

- The 343-instance grid is grouped into 64 clusters
  (`VulkanContext::CLUSTER_DIM=2`, `CLUSTERS_PER_AXIS=4`,
  `CLUSTER_COUNT=64`), by linear grid index, not spatial proximity.
- `cullingCoarse.comp` — a new compute shader, `local_size_x=64`,
  dispatched `(1,1,1)` (one workgroup covers all 64 clusters). One thread
  per cluster: tests that cluster's CPU-computed bounding sphere against
  both the camera and light frustums (the same `FrustumData`/
  `LightFrustumData` UBOs `culling.comp` already binds at 7/10), writing
  a direct indexed `0u`/`1u` into two new output buffers,
  `ClusterVisibleCamera`/`ClusterVisibleLight` (bindings 12/13) — no
  atomics needed, one thread owns one non-contended slot.
- `VulkanContext::clusterBuffer_` — 64 cluster bounding spheres,
  recomputed and re-uploaded every frame in `updateInstanceSimulation()`
  right next to the existing `objectBuffer_` upload (center = member
  mean, radius = max member `dist(center,member)+boundingSphereRadius_`
  — an enclosing sphere that strictly contains every member sphere, the
  property the correctness proof in TECHNICAL_NOTES depends on). Same
  "recompute CPU-side, reupload the SSBO every frame" discipline
  `objectBuffer_` already established (see "Grid collision + scatter"
  below).
- `GPUCullingPass` (`FrameRenderer.cpp`) now records **two** dispatches
  instead of one: `computePipelineCoarse_` (`(1,1,1)`) then a
  `VkMemoryBarrier` (`SHADER_WRITE→SHADER_READ`,
  `COMPUTE_SHADER→COMPUTE_SHADER` — the codebase's first compute→compute
  barrier, same hand-written/blanket shape as every other barrier in
  `drawFrame()`) then `computePipeline_` (the existing fine dispatch,
  unchanged dispatch size). Both pipelines share one `ComputeDescriptor`
  set/layout — `VulkanComputePipeline::create()` gained a `shaderPath`
  parameter so a second pipeline instance could point at
  `cullingCoarse.comp.spv`.
- `VulkanContext::clusterVisibleCameraBuffer_`/`clusterVisibleLightBuffer_`
  — HOST_VISIBLE like every other compute SSBO here, downloaded each
  frame in `FrameRenderer::drawFrame()` at the same safe post-fence-wait
  point the LOD visible counts already use, summed, and shown as
  "Clusters visible (camera/light): N / 64" in the "GPU Culling Stats"
  ImGui window.

### Tunable LOD thresholds (Phase 12, screen-space since Phase 14)

`culling.comp`'s LOD1/LOD2 cutoffs used to be `const float` shader
constants (Phase 12 made them runtime-tunable `FrustumPlanes` data
instead — see `TECHNICAL_NOTES.md` §26 — but still a flat world-space
distance pair); Phase 14 (`TECHNICAL_NOTES.md` §32) changed what that
data *means*, from a raw distance to a screen-space projected size in
pixels, without reintroducing a shader constant.

`FrustumPlanes` (`include/vulkan/culling/Frustum.h`) carries a 4th
field, `lodParams`: `x` = LOD1 screen-size threshold (px), `y` = LOD2
screen-size threshold (px), `z` = the frame's screen projection scale
(`sceneHeightPx / (2*tan(fovY/2))`, derived every frame in
`GPUCullingPass` from `Camera::FOV_DEGREES` — the same vertical FOV
`Camera::getProjectionMatrix()` itself uses — and
`VulkanSceneColorTarget::HEIGHT`, the fixed offscreen render target's
height), `w` unused. `culling.comp` derives each object's approximate
on-screen size as `radius * lodParams.z / camDist` (small-angle
approximation) and compares that against `lodParams.x`/`.y`, instead of
comparing raw distance against a flat threshold — so the same threshold
means the same apparent size regardless of camera FOV or output
resolution, where a flat world-unit distance didn't.
`VulkanContext::lod1ScreenSize()`/`lod2ScreenSize()`/setters keep
`lod1ScreenSize_ >= lod2ScreenSize_` (the inverse of the old distance
invariant — screen size shrinks with distance, so LOD1's threshold, the
closer/bigger boundary, must stay the larger pixel value). Two sliders
in the "GPU Culling Stats" ImGui window (next to the LOD0/1/2 visible
counts they control) drive it live, no shader recompile.
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
    ├── VulkanSceneColorTarget / VulkanDepthBuffer (sceneColorDepth_) /
    │     VulkanRenderPass (offscreen-color) / VulkanFramebuffer - the
    │     offscreen scene target GeometryPass now renders into, sampled by
    │     ImGui's "Viewport" window (see "Dockable viewport" module notes)
    ├── VulkanDescriptor (graphics, 7 bindings: UBO / albedo / SceneData /
    │     shadow map / normal / metallic-roughness / AO combined-image-samplers)
    ├── ComputeDescriptor (compute, 14 bindings: object / 3×visible / 3×indirect /
    │     camera frustum / shadow-visible / shadow-indirect / light frustum /
    │     cluster bounds / cluster-visible-camera / cluster-visible-light)
    ├── lods_[3] : LODMesh { VertexBuffer, IndexBuffer, VisibleInstanceBuffer, IndirectDrawBuffer }
    ├── ObjectBuffer (shared, 343 entries, re-uploaded every frame) / FrustumBuffer
    ├── shadowVisibleInstanceBuffer_ / shadowIndirectDrawBuffer_ / lightFrustumBuffer_
    │     (shadow pass's light-frustum-culled instance set, same shape as a
    │      single LOD's {VisibleInstanceBuffer, IndirectDrawBuffer} - see
    │      "Light-frustum culling for the shadow pass" module notes)
    ├── computePipelineCoarse_ / clusterBuffer_ / clusterVisibleCameraBuffer_ /
    │     clusterVisibleLightBuffer_ (hierarchical culling's coarse pass -
    │     64 clusters, re-uploaded every frame like ObjectBuffer - see
    │     "Hierarchical / two-stage GPU culling" module notes)
    ├── cachedInstances_ / instanceCurrentPositions_ / instanceVelocities_
    │     (rest formation / live scatter state, all 343 entries — see
    │      "Grid collision + scatter" module notes)
    ├── Material (material_ - bundles 4 VulkanTexture: albedo/normal/
    │     metallic-roughness/AO, see "Lighting" module notes)
    ├── environmentCubemap_ / skyboxDescriptor_ / skyboxPipeline_ (IBL
    │     Milestone 1 - baked-once procedural sky cubemap + the live
    │     skybox draw that samples it, see "Image-Based Lighting" module
    │     notes)
    ├── irradianceCubemap_ (IBL Milestone 2 - baked-once diffuse
    │     irradiance cubemap, see "Image-Based Lighting" module notes)
    ├── prefilteredCubemap_ / brdfLut_ / iblDescriptor_ (IBL Milestone 3 -
    │     baked-once specular-prefiltered mip-chain cubemap + BRDF LUT,
    │     bundled with irradianceCubemap_ into the 3-binding
    │     iblDescriptor_ bound as pipeline_'s set 1 - IBL is now complete)
    ├── sceneDataBuffer_ (shared UBO: light direction/color/intensity, camera pos,
    │     lightViewProj, shadow bias — bound at binding 2 on both descriptor_ and
    │     projectileDescriptor_, now also read by the vertex stage)
    ├── Projectile (plain C++, position/direction/speed/lifetime)
    ├── projectileUniformBuffer_ / projectileDescriptor_ / projectileInstanceBuffer_
    │     (own UBO+descriptor+1-entry instance buffer — reuses lods_[2]'s mesh)
    └── Camera
        └── FrameRenderer
            └── FrameGraph
                ├── GPUCullingPass (Compute) — coarse per-cluster frustum
                │                    test (both camera and light), then
                │                    fine camera frustum test + LOD
                │                    fan-out, plus an independent light-
                │                    frustum test compacting shadow casters,
                │                    both fine tests gated on the coarse
                │                    pass's per-cluster flags
                ├── ShadowPass (Shadow) — depth-only, draws the light-
                │                          frustum-culled instance set via
                │                          vkCmdDrawIndexedIndirect
                ├── GeometryPass (Graphics, reads CullingPass + ShadowPass,
                │   │                       renders into sceneFramebuffer_) —
                │   │                       1× vkCmdDraw (skybox, drawn
                │   │                       first, depth test/write off) +
                │   │                       per-draw push constant
                │   │                       (MaterialPushConstants: albedo/metallic/roughness)
                │   │                       + 3× vkCmdDrawIndexedIndirect
                │   │                       + 1× vkCmdDrawIndexed (projectile, if active)
                └── ImGuiPass (UI, reads GeometryPass, runs in the
                                              swapchain's own render pass) —
                                              dockable "Viewport" window
                                              (samples sceneColorTarget_) +
                                              debug windows (see "Dockable
                                              viewport" module notes)
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
Wait Fence → Read back previous frame's 3 LOD instance counts + coarse-
pass cluster-visible counts (debug overlay) + this frame slot's 4 GPU
timestamps → Culling/Shadow/Graphics/Total ms (if the GPU supports
timestamp queries — see TECHNICAL_NOTES §23)
        │
Acquire Swapchain Image
        │
Reset IndirectLOD0/1/2.instanceCount = 0   (3× CPU write)
Reset ShadowIndirect.instanceCount = 0     (CPU write, indexed by LOD0)
        │
Reset this frame's VkQueryPool → write timestamp 0 (TOP_OF_PIPE)
        │
Compute: GPUCullingPass (see TECHNICAL_NOTES §31)
   (upload camera frustum + light frustum (zeroToOne=true, see
    TECHNICAL_NOTES §28) →
    [3a] Coarse dispatch (1,1,1), 64 threads = 64 clusters: sphere test
    each cluster's bounding sphere (clusterBuffer_, re-uploaded every
    frame from instanceCurrentPositions_) vs. both frustums, write
    ClusterVisibleCamera/ClusterVisibleLight flags →
    compute→compute VkMemoryBarrier (SHADER_WRITE→SHADER_READ) →
    [3b] Fine dispatch (OBJECT_COUNT+63)/64, 343 threads: each thread
    reads its cluster's flags first and skips the corresponding plane
    test entirely if that cluster was coarse-rejected, otherwise same as
    before - light-frustum survivors compact into ShadowVisible/
    ShadowIndirect unconditionally; camera-frustum survivors additionally
    get distance-based LOD bucketing, atomic compaction per bucket)
        │
Write timestamp 1 (BOTTOM_OF_PIPE) — closes the Culling interval
        │
Memory Barrier (SHADER_WRITE → VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ)
        │
Begin Shadow Render Pass (own framebuffer, fixed 2048×2048 resolution)
        │
Shadow: ShadowPass
   push {lightViewProj, spin rotation} → bind LOD0 vertex/index buffers +
   shadowVisibleInstanceBuffer_ → vkCmdDrawIndexedIndirect against
   shadowIndirectDrawBuffer_ (GPU-supplied instance count) → if
   projectile active: push {lightViewProj, identity}, bind LOD2 mesh +
   its instance buffer, vkCmdDrawIndexed (unculled - single instance)
        │
End Shadow Render Pass
        │
Write timestamp 2 (BOTTOM_OF_PIPE) — closes the Shadow interval
        │
Image Memory Barrier (LATE_FRAGMENT_TESTS write → FRAGMENT_SHADER read)
        │
Begin Offscreen Scene Render Pass (sceneRenderPass_/sceneFramebuffer_,
   live-resized to the docked Viewport panel's size - see "Live-resized
   viewport target" module notes)
        │
Graphics: GeometryPass
   upload SceneData (light + camera + lightViewProj + shadow bias) →
   draw skybox (fullscreen triangle, samples environmentCubemap_, depth
   test/write off - drawn first so the grid naturally overdraws it,
   see "Image-Based Lighting" module notes) →
   bind pipeline_ → bind set 1 (iblDescriptor_: irradiance/prefiltered/
   BRDF LUT, once - stays bound for the rest of the pass) → push grid
   material constants → for each LOD: bind set 0 (descriptor_), bind
   vertex/index/visible-instance buffers, vkCmdDrawIndexedIndirect
   (GPU-supplied instance count, fragment shader samples the shadow map
   for occlusion and irradianceMap/prefilteredMap/brdfLUT for full
   split-sum ambient) →
   if projectile active: bind set 0 (projectileDescriptor_), push its own
   material constants, bind LOD2 mesh + its instance buffer,
   vkCmdDrawIndexed
        │
End Offscreen Scene Render Pass
        │
Image Memory Barrier (COLOR_ATTACHMENT_OUTPUT write → FRAGMENT_SHADER read)
        │
Begin Swapchain Render Pass (renderPass_/framebuffer_ - UI only, no 3D
   geometry draws here anymore)
        │
UI: ImGuiPass — DockSpaceOverViewport + dockable "Viewport" window
   (ImGui::Image samples sceneColorTarget_) + stats overlay + live
   lighting/shadow sliders + shadow map debug preview, all docked beside
   the Viewport instead of overlapping it
        │
End Swapchain Render Pass
        │
Write timestamp 3 (BOTTOM_OF_PIPE) — closes the Graphics interval
        │
Queue Submit → Present
```

