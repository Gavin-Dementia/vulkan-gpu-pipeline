# Renderer Roadmap

This document tracks the planned development stages of the project,
updated against what was actually built (vs. originally planned).

---

## Phase 0 — Vulkan Bootstrap

**Status: Complete**

Implemented:
- GLFW window
- Vulkan instance
- Validation layers
- Surface creation
- Physical device selection
- Logical device creation
- Swapchain creation

---

## Phase 1 — Frame Presentation

**Status: Complete**

Implemented:
- Command Pool
- Command Buffer (double-buffered, 2 frames in flight)
- Fence / Semaphore (per-frame + per-swapchain-image)
- Acquire / Submit / Present

Milestone reached: first presented frame (solid color clear).

---

## Phase 2 — First Triangle

**Status: Complete**

Implemented:
- Render Pass (later extended with a depth attachment — see Phase 3)
- Framebuffer
- Graphics Pipeline
- Shader Modules

Milestone reached: hardcoded triangle, color from fragment shader.

---

## Phase 3 — Mesh Rendering

**Status: Complete — exceeded original scope**

Originally planned: Vertex Buffer, Index Buffer, Uniform Buffer,
Descriptor Sets.

Actually implemented:
- Vertex Buffer (staging → DEVICE_LOCAL transfer)
- Index Buffer with vertex deduplication (OBJ loader: 2904 → 507 unique
  vertices for Suzanne)
- Uniform Buffer (MVP matrix, HOST_VISIBLE — written every frame, no
  staging; see `TECHNICAL_NOTES.md` §3 for why this differs from
  vertex/index buffer handling)
- Descriptor Sets (graphics: 1 binding; later compute: 4 bindings)
- Depth Buffer (`D32_SFLOAT`, not originally scoped for this phase but
  pulled in early since it was needed before instancing made draw order
  ambiguous)
- Normal-based fragment shading (to visually confirm depth testing was
  actually working, not just present)

Milestone reached: real OBJ mesh (Suzanne) rendered with correct depth
ordering and per-fragment normal coloring.

---

## Phase 4 — Scene Framework

**Status: Partially complete, descoped in favor of Phase 5**

Originally planned: Camera, Transform, Material, Scene Graph.

Actually implemented:
- Camera (first-person, WASD movement, shared view matrix between
  culling and rendering). Originally used QE + arrow keys for
  up/down/yaw/pitch; switched to mouse-look in Phase 7 (§18) once
  mouse input was already needed for the projectile's fire trigger.
- Transform: handled inline via per-instance position data rather than
  a general Transform component (no rotation/scale per-instance yet —
  all instances share the model matrix's rotation)

Not implemented: Material abstraction, general Scene Graph. Decision was
to prioritize the GPU-driven culling core (Phase 5) over generalizing
the scene representation, since culling was the project's actual stated
goal and a full scene graph wasn't a prerequisite for it.

---

## Phase 5 — GPU-Driven Rendering

**Status: Complete — exceeded original scope**

Originally planned: Storage Buffers, Indirect Draw, Compute-driven
visibility.

Actually implemented:
- Storage Buffers (`ObjectBuffer`, `VisibleInstanceBuffer`,
  `IndirectDrawBuffer` — all SSBOs)
- Compute-driven visibility: sphere-vs-frustum test (Gribb-Hartmann
  plane extraction from the view-projection matrix), 64 threads/workgroup
- **Atomic compaction** — not originally scoped, but turned out to be
  the actual mechanism that makes this GPU-driven rather than GPU-
  assisted. A naive 0/1 visibility array still requires the CPU to read
  results back and decide a draw count; atomic compaction lets passing
  threads claim output slots directly, so the GPU itself determines and
  writes the final instance count. See `TECHNICAL_NOTES.md` §7.
- Indirect Draw (`vkCmdDrawIndexedIndirect`) — CPU never reads culling
  results back
- FrameGraph extended to support Compute and Graphics passes as distinct
  stages within the same DAG, rather than special-casing compute outside
  the dependency graph (`TECHNICAL_NOTES.md` §4)
- 343-instance grid (7×7×7) to make culling behavior visually legible
- Experimentally validated: oscillating a test object's position with
  `sin(time)` produced matching `1 → 0 → 1` visibility transitions in
  sync, confirming the culling logic is functionally correct rather than
  coincidentally always passing

Milestone reached: GPU-driven culling pipeline, interactively verifiable
via camera movement (instances visibly appear/disappear as the frustum
moves).

---

## Phase 6 — GPU-Driven LOD

**Status: Complete**

Originally planned (as a Phase 5 follow-on): distance-based LOD
selection reusing the existing compute-pass infrastructure.

Actually implemented:
- 3 LOD mesh variants (`suzanne.obj` / `suzanne_lod1.obj` /
  `suzanne_lod2.obj`, 507 / 165 / 34 unique vertices)
- `culling.comp` extended to bucket each frustum-passing instance into
  one of 3 distance ranges (`< 12.0`, `< 20.0`, `>= 20.0`), each with its
  own atomic-compaction output pair (`VisibleLODN`, `IndirectLODN`) —
  see `TECHNICAL_NOTES.md` §15 for why this needed 3 full parallel sets
  rather than one buffer with a per-instance LOD tag
- `ComputeDescriptor` grew from 4 to 8 bindings; `GeometryPass` now
  issues 3 `vkCmdDrawIndexedIndirect` calls per frame, one per LOD mesh
- ImGui overlay extended to show LOD0/1/2 visible counts individually

Not yet done at the time: LOD distance thresholds were hardcoded shader
constants, not derived from mesh detail or screen-space size, and not
runtime-tunable (fixed in Phase 12); texture sampling still isn't
reunited with the (still UV-less) Suzanne LOD chain (Phase 8 milestone 2
later textured LOD0 with a different mesh - see its section below).

Milestone reached: instances visibly swap mesh detail level as the
camera moves toward/away from them (see `docs/assets/lod_demo_01.gif`).

---

## Phase 7 — Interactive Objects (toward PBR)

**Status: Complete (both milestones)**

Stated long-term direction: PBR material model. First concrete step
toward it is an interactive object the player can aim and fire, which
collides with and scatters the instance grid — the feature that
originally motivated this whole arc.

Milestone 1 — mouse-fired projectile — implemented:
- `Projectile` (plain C++ class, no Vulkan) — `launch()`/`update()`,
  constant-velocity straight-line motion, fixed lifetime expiry
- Left-click (polled, not a GLFW callback — ImGui already owns those)
  fires along the camera's existing look direction; gated on
  `ImGui::GetIO().WantCaptureMouse` so clicking the debug overlay
  doesn't also fire a shot
- Renders by reusing LOD2's existing mesh + its own UBO/descriptor
  set/instance buffer — see `TECHNICAL_NOTES.md` §17 for why a second
  UBO is required (not just cleaner) once two different model matrices
  need to coexist in one frame's command buffer
- No new material/texture — reuses the single shared texture, since
  material/texture choices are explicitly deferred to the PBR work
- Camera aiming switched from QE + arrow keys to mouse-look (window set
  to `GLFW_CURSOR_DISABLED`) — see `TECHNICAL_NOTES.md` §18. Coexists
  with the projectile's left-click fire trigger since mouse buttons are
  independent of cursor visibility mode.

Milestone 2 — grid collision + scatter — implemented:
- `objectBuffer_` (the 343-entry bounding-sphere buffer `culling.comp`
  already reads every dispatch) goes from write-once at startup to
  re-uploaded every frame from CPU-simulated positions — zero compute
  shader or descriptor changes needed, since the shader never had a
  separate concept of a "static" grid position. Cheap thanks to
  `VulkanBuffer`'s persistent mapping. See `TECHNICAL_NOTES.md` §20.
- CPU simulation (position + velocity + framerate-independent damping,
  not full rigid-body physics): on the projectile touching any instance,
  applies a radially-falling-off blast impulse to every instance within
  a blast radius (not just the one touched), then stops the projectile —
  one explosion per flight
- Manual **R** key resets the grid to its original formation (no
  automatic spring-back — instances stay scattered until reset)
- Mutual instance-vs-instance collision, every frame — fixes scattered
  instances visually clipping through each other once settled; see
  `TECHNICAL_NOTES.md` §21. Originally a pure positional pushout, since
  superseded by a velocity-impulse + light positional-correction hybrid
  (§30) that lets colliding instances actually deflect/bounce off each
  other, with an ImGui-tunable restitution
- Bundled in: manual **T** key pauses/resumes the grid's shared spin,
  useful for observing the scatter without the whole grid also rotating

---

## Phase 8 — PBR Lighting

**Status: Both milestones complete**

First concrete PBR step, scoped narrowly on purpose: get real lighting
math on screen before touching materials/textures at all.

Milestone 1 — Cook-Torrance BRDF — implemented:
- GGX distribution, Smith geometry, Fresnel-Schlick, one directional
  light — see `TECHNICAL_NOTES.md` §19
- `SceneData` (light + camera data) added as a 3rd binding on the
  existing graphics descriptor set, shared by every material (grid and
  projectile both reference the same buffer) — no new descriptor set
- Material params (albedo/metallic/roughness) as a fragment-stage push
  constant, re-issued per draw call — grid and projectile now render
  with visibly different materials (rough dielectric vs. shiny metal),
  proving the mechanism actually varies per-draw
- Fixed a previously-invisible bug surfaced by adding real lighting:
  `triangle.vert` wasn't rotating normals by the model matrix
  (`fragNormal = inNormal` instead of `mat3(ubo.model) * inNormal`) —
  invisible under the old flat unlit tint, would have made the lit
  appearance swim independently of the grid's existing spin
- Avoided a double-gamma bug: swapchain is `VK_FORMAT_B8G8R8A8_SRGB`
  (GPU auto-converts linear→sRGB on write), so the shader does a
  Reinhard tonemap only, no manual `pow(1/2.2)` gamma step
- ImGui "Lighting" window: real-time direction/color/intensity sliders

Milestone 2 — texture-based materials — implemented:
- New `Material` class (`include/vulkan/texture/Material.h`) bundling 4
  `VulkanTexture` instances (albedo/normal/metallic-roughness/AO) —
  exactly the "formal Material class, generalizing the per-object-
  descriptor-set pattern" this phase's plan named as the goal. Grid and
  projectile still share one `Material` instance (same "reuses the
  single shared texture" reasoning milestone 1 already established).
- `VulkanTexture::create()` gained a `VkFormat` parameter — albedo stays
  `VK_FORMAT_R8G8B8A8_SRGB` (color data), normal/metallic-roughness/AO
  use `VK_FORMAT_R8G8B8A8_UNORM` (non-color data must not be gamma-
  decoded on sample). `VulkanDescriptor` grew from 4 to 7 bindings.
- `triangle.frag`: metallic/roughness now sample a glTF-convention
  metallic-roughness map (G=roughness, B=metallic) multiplied by the
  existing push-constant factors — same "factor × texture" pattern
  `finalAlbedo` already used, so grid vs. projectile keep looking
  visually distinct. AO multiplies the ambient term only. Normal mapping
  uses a derivative-based tangent frame (`dFdx`/`dFdy` on world position
  + UV) instead of a precomputed tangent vertex attribute — zero
  `Vertex`/`ObjLoader` changes needed. See `TECHNICAL_NOTES.md` §25 for
  the full design writeup, including a real NaN bug this technique hit
  and how it was fixed.
- **Blocking problem, solved:** Suzanne (all 3 LOD `.obj` files) had zero
  real UV data. LOD0 now loads `assets/suzanne_pbr.obj`, a UV/normal-
  mapped re-export of the same base mesh sourced from
  `opengl-tutorials/ogl` (asked the user, who directed a GitHub search
  over switching demo assets or hand-rolling triplanar projection) — no
  explicit LICENSE file exists in that source repo, so the user's chosen
  resolution was clear attribution rather than treating the community
  convention as a substitute for one: both the asset file itself and
  `README.md`'s new "Asset Credits" section now name the exact source
  URL.
- **Known, explicitly accepted gap:** LOD1/LOD2 still use the original
  UV-less meshes (matching UV-preserving decimations need a 3D tool this
  environment doesn't have) — they render exactly as before, not a
  regression, just not textured. **Still open** — see Phase 20's notes,
  which searched for a fix and left this one alone. Also placeholder
  textures throughout (`assets/normal.png`, `metallic_roughness.png`,
  `ao.png` are small self-generated PNGs — flat normal, a
  metallic/roughness gradient, flat AO — not sourced/authored PBR photo
  sets), same "good enough now, refine later" bar this project already
  applies elsewhere (LOD thresholds, the flat ambient term).
  **Later revisited and closed — Phase 20.**

---

## Phase 9 — Shadow Mapping

**Status: Complete**

First physical light-occlusion step: classic shadow mapping for the
existing single directional light, chosen over an analytic
ray-sphere-occlusion alternative that would have reused `culling.comp`'s
bounding spheres but given geometrically-wrong (blobby) shadow edges —
see `TECHNICAL_NOTES.md` §22.

Milestone 1 — shadow render target + depth-only pass — implemented:
- `VulkanShadowMap` (fixed 2048×2048, sampled `D32_SFLOAT`), depth-only
  variants of `VulkanRenderPass`/`VulkanFramebuffer`, and a new
  `VulkanShadowPipeline` (vertex-only, a `ShadowPushConstants` push
  constant — grew a second `mat4` field after the spin-rotation bug
  below was found)
- `FrameGraph` gained a third pass stage, `PassStage::Shadow` /
  `executeShadow()`, alongside `Compute`/`Graphics` — mirrors how
  `executeCompute()` already runs outside the main render pass with its
  own explicit wrapper in `FrameRenderer::drawFrame()`
- `ShadowPass` originally drew all 343 grid instances unculled by
  binding `objectBuffer_` directly as the instance buffer; superseded by
  light-frustum culling (see below)
- Verified via a temporary "Shadow Map" ImGui debug window
  (`ImGui_ImplVulkan_AddTexture`) before any shading code depended on it

Milestone 2 — sample the shadow map in shading — implemented:
- `SceneData` gained `lightViewProj`; `triangle.vert` now also reads
  binding 2 (previously fragment-only) to output `fragLightSpacePos`
- `VulkanDescriptor` gained binding 3 (shadow map combined-image-sampler)
  on both `descriptor_` and `projectileDescriptor_`
- `triangle.frag`'s `calcShadow()` multiplies only the direct `Lo` term —
  the flat ambient term stays unshadowed

Milestone 3 — PCF softening + tunable bias — implemented:
- `SceneData.shadowParams.x` (base bias) exposed as an ImGui slider in
  the existing "Lighting" window
- 3×3 PCF in `calcShadow()`, fixing the pixel-level shadow acne seen on
  directly-lit surfaces (most visible on the grid's top layer, which has
  the least self-occlusion to mask it) in Milestone 2

Three real bugs surfaced and fixed along the way (all in
`TECHNICAL_NOTES.md` §22): `objectBuffer_` was missing
`VK_BUFFER_USAGE_VERTEX_BUFFER_BIT` before it could be reused as an
instance buffer; the light's orthographic matrix needed
`glm::orthoRH_ZO()` instead of `glm::ortho()` — this project's default
GLM depth convention is silently harmless for `Camera`'s perspective
matrix but was clipping away the near half of the light's frustum
outright for an orthographic one; and `shadow.vert` never applied the
grid's spin rotation the way `triangle.vert` does, so the shadow map was
permanently cast from each mesh's un-rotated rest pose while the visible
geometry kept spinning — fixed by adding a `model` field to
`ShadowPushConstants` and pushing the same per-draw rotation
`GeometryPass` already computes.

Milestone 4 — light-frustum culling — implemented (`TECHNICAL_NOTES.md`
§28): `culling.comp` now also runs an independent 6-plane sphere test
against the light's orthographic frustum for every object (unconditional
on camera visibility, since a shadow caster can be off-screen), compacting
survivors into a 4th output pair (`ShadowVisible`/`ShadowIndirect`,
`ComputeDescriptor` bindings 8–9) alongside the 3 camera-side LOD buckets.
`ShadowPass` switched from an unculled direct draw to
`vkCmdDrawIndexedIndirect` against that compacted set. Required first
re-verifying the camera path's `FrustumPlanes::extractFromMatrix` was
itself correct, then giving it a `zeroToOne` parameter before reusing it
for the light — naively reusing the camera's `[-1,1]`-convention
near-plane formula on the light's `[0,1]` `orthoRH_ZO()` matrix would have
silently produced a near plane that never culls anything, the same
"silent, not loud" failure shape as the original ortho bug in Milestone 1
above.

Milestone 5 — dynamic scene radius — implemented (`TECHNICAL_NOTES.md`
§29): `lightViewProj()`'s `kSceneRadius` is no longer a fixed constant.
It's now computed fresh every frame as the max distance of any grid
instance from the origin plus `boundingSphereRadius_` margin, floored at
`kMinSceneRadius = 24.0f` (the old constant, preserved as the rest-
formation floor so nothing changes until a blast actually scatters the
grid). A blasted instance now grows the light's frustum to match instead
of silently falling outside it — and since Milestone 4's light-frustum
culling reads `lightViewProj()` fresh every frame too, the dynamic sizing
flows through to the culling test automatically, no extra wiring needed.
The projectile's own position is deliberately excluded from the radius
calculation (short-lived, single instance, not worth the coupling).

---

## Phase 10 — GPU Timestamp Performance Instrumentation

**Status: Complete**

Turns "GPU-driven culling is cheap" from a claim into a number. Closes an
open item that had been carried since Phase 5 — see `TECHNICAL_NOTES.md`
§23.

- 4 `vkCmdWriteTimestamp` markers per frame, one at each of the
  `FrameGraph`'s 3 stage boundaries (frame start, compute end, shadow
  end, graphics end) plus the implicit start — reported as 3 intervals
  (Culling / Shadow / Graphics) and a total, shown live in the existing
  "GPU Culling Stats" ImGui window
- One `VkQueryPool` per frame-in-flight, reusing the exact safe-readback
  timing already established for the LOD visible-instance counts: a
  frame slot's `vkWaitForFences` already guarantees its previous GPU work
  is done, so reading that slot's query results right there is free —
  no `VK_QUERY_RESULT_WAIT_BIT`, no extra stall
- `VulkanDevice` now queries `timestampComputeAndGraphics` /
  `timestampValidBits` once at device-creation time and degrades
  gracefully (skips pool creation, shows "N/A" in the UI) rather than
  assuming every GPU supports timestamp queries

---

## Phase 11 — Dockable Editor UI

**Status: Complete**

The 3 debug windows (`GPU Culling Stats` / `Lighting` / `Shadow Map`) used
to be plain `ImGui::Begin()` windows with no assigned position, cascading
on top of the rendered grid. Reworked into a real editor-style layout so
the debug UI is an extra area alongside the 3D view, not overlapping it.

- `third_party/imgui` repointed from a non-docking commit to the
  `docking` branch (`ImGuiConfigFlags_DockingEnable` /
  `ImGui::DockSpaceOverViewport` didn't exist before this). No new source
  files — docking lives entirely inside `imgui.cpp`/`imgui_internal.h`,
  already covered by `CMakeLists.txt`'s existing imgui source list.
- New offscreen scene render target (`VulkanSceneColorTarget`, fixed
  1280×720 — matches `Camera::ASPECT_RATIO`'s existing fixed-window
  assumption) + a new `VulkanRenderPass::createOffscreenColor()`, modeled
  directly on the shadow map's depth-only precedent (§13/§22 pattern:
  sampled render target, own render pass, own framebuffer).
  `GeometryPass`/`LightingPass`/`PostProcess` (`PassStage::Graphics`) now
  render here instead of directly to the swapchain.
- `FrameGraph` gained a 4th `PassStage`, `UI` — the swapchain's own
  render pass now hosts *only* `ImGuiPass`, following the exact
  extension pattern `docs/setup.md` §8 already documents ("a genuinely
  new stage needs a new `PassStage` value + `execute*()` + a wrapper in
  `drawFrame()`, following the Shadow stage as the template").
- `ImGuiPass` calls `ImGui::DockSpaceOverViewport()` and a one-time
  `DockBuilder*` layout (Viewport centered, the 3 debug windows docked as
  a tabbed group on the right) so the first run already shows the
  intended layout instead of default-cascaded windows. The scene color
  target is registered with `ImGui_ImplVulkan_AddTexture()` and displayed
  in a new "Viewport" window — the same mechanism the Shadow Map debug
  preview already used, aimed at the main color output instead of the
  shadow depth map.
- Deliberately **not** dynamically resized to the panel's pixel size —
  `ImGui::Image()` just scales the fixed-resolution texture to whatever
  size the docked panel ends up being. This codebase has no swapchain
  resize handling anywhere else either, so building live-resize plumbing
  for just this one target was scoped out. See `TECHNICAL_NOTES.md` §24
  for the full Qt-vs-ImGui-docking tradeoff analysis behind this choice.
  **Later revisited and closed — Phase 16.**

**Addendum — a real interaction bug surfaced afterward, user-reported:**
docking the debug windows over the entire client area made a pre-existing
cursor-mode/ImGui interaction gap (§18 addendum) into a frequent,
noticeable bug — the projectile's left-click trigger would intermittently
not register right after releasing Ctrl, only reliably firing again after
a large mouse-look swing. Root cause: ImGui's GLFW backend keeps feeding
the disabled-cursor's unbounded virtual position into its own hit-testing
regardless, and now that virtually the whole screen is a docked ImGui
window, that stale position is almost always still "inside one." Fixed
by syncing `ImGuiConfigFlags_NoMouse` with cursor visibility every frame —
see `TECHNICAL_NOTES.md` §27.

---

## Phase 12 — Tunable LOD Distance Thresholds

**Status: Complete**

Closes the "Not yet done" item Phase 6 originally carried: `culling.comp`'s
`LOD1_DIST`/`LOD2_DIST` were hardcoded shader constants, meaning any
change needed a shader recompile - not something to iterate on while
looking at the scene.

- `FrustumPlanes` (`include/vulkan/culling/Frustum.h`) grew a 4th field,
  `lodDistances` (`x = LOD1_DIST, y = LOD2_DIST`), alongside the existing
  6 frustum planes + camera position - uploaded to the GPU every frame
  as part of the same buffer, not a new binding. `culling.comp`'s
  `FrustumData` uniform block gained the matching field; the two former
  `const float` shader constants are gone.
- `VulkanContext::lod1Distance()`/`lod2Distance()` (default 12.0/20.0,
  matching the old hardcoded values) with setters that keep
  `lod2Distance_ >= lod1Distance_` - `culling.comp`'s `if (camDist <
  LOD1) ... else if (camDist < LOD2) ...` chain silently misbehaves if
  the thresholds cross, so the setters enforce the invariant rather than
  trusting every call site to.
- Two new sliders ("LOD1 Distance" / "LOD2 Distance") in the existing
  "GPU Culling Stats" ImGui window, right next to the LOD0/1/2 visible
  counts they control - same "expose it, easier to find by eye than to
  compute" reasoning as the shadow bias slider (§Phase 9) and the
  lighting sliders (§Phase 8).
- Verified interactively: dragging LOD1 Distance up past every
  instance's camera distance reclassifies the entire grid to LOD0 live,
  visibly swapping every instance to the higher-detail mesh with no
  restart.

Not yet done at the time: thresholds were still a flat distance in world
units, not derived from mesh screen-space size - closed by Phase 14
below. Still not derived from per-mesh detail level, and still a
manually-tuned pair of numbers rather than an automatically-computed one.

---

## Phase 13 — Hierarchical (Coarse + Fine) GPU Culling

**Status: Complete**

Closes the "Multi-pass / hierarchical culling" item this roadmap had
carried since Phase 5/6 under "Open / not yet started." `culling.comp`
(the flat 343-thread scan) now runs as the **fine** pass behind a new
**coarse** pass, `cullingCoarse.comp`, that rejects whole clusters of
instances against both frustums before the fine pass does any per-object
work.

- The grid is grouped into 64 clusters (`CLUSTER_DIM=2`,
  `CLUSTERS_PER_AXIS=4`), by linear grid index rather than spatial
  proximity. Per-cluster bounding spheres are recomputed and re-uploaded
  every frame in `updateInstanceSimulation()`, the same "CPU-side
  recompute, reupload the SSBO every frame" discipline `objectBuffer_`
  already established (Phase 7 milestone 2).
- `cullingCoarse.comp` (new, `local_size_x=64`, dispatched `(1,1,1)`):
  one thread per cluster, tests both the camera and light frustums
  (reusing the existing `FrustumData`/`LightFrustumData` UBOs), writes
  two flag buffers with a direct indexed write (no atomics needed).
- `culling.comp` gates its existing per-object camera/light plane tests
  behind those flags, independently (an object can be light-visible/
  camera-invisible or vice versa). See `TECHNICAL_NOTES.md` §31 for a
  proof this can never change the final visible set - the coarse pass
  only ever skips work that would have been culled anyway.
- `GPUCullingPass` now records 2 dispatches with a new compute→compute
  `VkMemoryBarrier` between them (this codebase's first barrier of that
  shape - every prior one was compute→graphics or graphics→graphics).
- `ComputeDescriptor` grew 11→14 bindings; both compute pipelines share
  one descriptor set. `VulkanComputePipeline::create()` gained a
  `shaderPath` parameter so a second pipeline instance could target the
  new shader.
- New "Clusters visible (camera/light): N / 64" counts in the "GPU
  Culling Stats" ImGui window, read back the same safe post-fence-wait
  way as the existing LOD counts.

**Honest scale note:** at 343 instances / 6 fine workgroups this has no
measurable performance payoff - the value is closing the named
architectural gap and correctly demonstrating the two-stage GPU-driven
culling pattern this project's long-term direction calls for, not a perf
win at this instance count.

**Accepted limitation:** cluster membership is static/index-based, not
re-clustered by proximity - a heavily-scattered projectile blast (Phase
7 milestone 2) can make a cluster's bounding sphere balloon toward the
whole scene, degrading the coarse pass's rejection efficacy (correctness
is unaffected). See `TECHNICAL_NOTES.md` §31.

---

## Phase 14 — Screen-Space LOD Thresholds

**Status: Complete**

Closes the "Not yet done" item Phase 12 carried: LOD selection compared
raw world-space camera distance against a flat threshold pair, which
means the same threshold implies a different apparent on-screen size at
a different FOV or output resolution - not meaningful across scenes with
different camera setups, even though the values themselves were already
runtime data (Phase 12), not shader constants.

- `culling.comp` now derives each object's approximate on-screen size
  from its bounding sphere, camera distance, and a screen projection
  scale (`radius * screenScale / camDist`, the standard small-angle
  approximation for a sphere's projected pixel size), and compares that
  against the LOD1/LOD2 thresholds instead of comparing raw distance.
- `FrustumPlanes::lodDistances` renamed to `lodParams`; `x`/`y` are now
  screen-size thresholds in pixels, `z` is the frame's screen projection
  scale (`sceneHeightPx / (2*tan(fovY/2))`, derived every frame in
  `GPUCullingPass` from `Camera::FOV_DEGREES` - the same vertical FOV
  `Camera::getProjectionMatrix()` already uses, now made public for this
  - and `VulkanSceneColorTarget::HEIGHT`, the fixed offscreen render
  target the scene actually rasterizes into).
- `VulkanContext::lod1Distance()`/`lod2Distance()` renamed to
  `lod1ScreenSize()`/`lod2ScreenSize()` (default 120px/60px); setters now
  keep `lod1ScreenSize_ >= lod2ScreenSize_`, the inverse of the old
  distance invariant, since screen size shrinks with distance rather than
  growing with it.
- ImGui sliders relabeled "LOD1/LOD2 Screen Size" (px), same live-tuning
  UX Phase 12 established, no shader recompile needed.
- Verified interactively: at a fixed camera distance, moving the camera
  closer/farther now crosses the LOD boundary at a screen-size-consistent
  point regardless of which direction the grid is viewed from, rather
  than a fixed radius around the camera that didn't account for how large
  an object actually appears.

Not yet done: still not derived from per-mesh detail level (a low-poly
LOD2 mesh and a high-poly LOD0 mesh use the same threshold pair even
though their "acceptable" screen size arguably differs) - still a
manually-tuned pair of numbers, just now a resolution/FOV-independent one.

---

## Phase 15 — Image-Based Lighting

**Status: Complete (Milestones 1-3 of 3)**

Full IBL (diffuse irradiance convolution + specular prefilter/BRDF LUT)
was requested but explicitly staged across multiple milestones rather
than built in one pass.

**Milestone 1** — the cubemap infrastructure, a procedurally baked sky
(no external HDR asset - a deliberate choice, see `TECHNICAL_NOTES.md`
§33), and a visible skybox proving the pipeline is wired correctly:

- `VulkanCubemap` (new) - this project's first cube image: 1 sampling
  view (`VK_IMAGE_VIEW_TYPE_CUBE`) + 6 per-face render-target views
  (`VK_IMAGE_VIEW_TYPE_2D`), single mip level.
- `VulkanContext::initEnvironment()` bakes a procedural gradient sky
  (bright sun disk aligned with the existing `lightDirection_`) into the
  cubemap once at startup via 6 draws into a locally-scoped render pass/
  framebuffer/pipeline, reusing `VulkanTexture`'s existing one-shot
  command buffer pattern.
- New `VulkanRenderPass::createColorOnly()` / `VulkanFramebuffer::
  createColorOnly()` (no-depth variants), a shared
  `shaders/fullscreenTriangle.vert`, `VulkanEnvCapturePipeline` (bake) and
  `VulkanSkyboxPipeline` (live draw, inside `GeometryPass`, depth test/
  write disabled, no new `FrameGraph` pass).
- Direction reconstruction (both bake and live skybox) uses
  `inverse(viewProj)`, not hand-derived per-face basis vectors - see
  `TECHNICAL_NOTES.md` §33 for why.

**Milestone 2** — diffuse irradiance convolution, actually replacing the
diffuse half of `triangle.frag`'s old flat `0.03 * albedo * ao` ambient
term (see `TECHNICAL_NOTES.md` §34):

- `irradianceCubemap_` (32×32/face - diffuse irradiance is extremely
  low-frequency, no need for `environmentCubemap_`'s 512×512) baked via
  `shaders/irradianceConvolve.frag`, the standard cosine-weighted
  hemisphere Riemann-sum integral - a trusted reference derivation, same
  discipline as Milestone 1's capture-face table.
- Both bakes (environment + irradiance) now run in one command buffer/
  one submit, with a mid-buffer memory barrier between them so the
  irradiance draws can safely sample the just-baked environment.
- `VulkanPipeline` grew a second descriptor set (this codebase's first
  multi-set pipeline layout): set 0 stays the grid/projectile's material
  data, set 1 is new ambient-lighting data (the irradiance cubemap),
  bound once per frame and shared by both draws.
- `SkyboxDescriptor` renamed to `CubeSamplerDescriptor` - its shape was
  always generic, and this milestone gave it a second real use site
  (the irradiance bake's input, plus the new set-1 binding).
- `triangle.frag`'s ambient term is now real diffuse IBL
  (`kD_ambient * irradiance * finalAlbedo * ao`, using an `NdotV`-based
  Fresnel split since there's no single incident direction for ambient)
  - specular IBL is still not computed.

**Milestone 3** — specular prefilter + BRDF LUT, Karis's split-sum
specular IBL approximation, completing the ambient term (see
`TECHNICAL_NOTES.md` §35):

- `VulkanCubemap` gained mip-chain support (backward-compatible -
  `mipLevels` defaults to 1, M1/M2's cubemaps unchanged). Fixed a real,
  previously-silent sampler bug in the process: `minLod`/`maxLod` were
  never set, which clamps every sampled LOD to 0 regardless of what a
  shader requests - harmless at 1 mip, would have been a real bug the
  moment a multi-mip cubemap was sampled.
- `prefilteredCubemap_` (5 mips, 128×128 down to 8×8, roughness
  0.0/0.25/0.5/0.75/1.0) baked via `shaders/prefilterEnv.frag`
  (GGX-importance-sampled, Karis/Epic's reference technique) - 5
  short-lived `VulkanPrefilterPipeline` instances, one per mip (this
  codebase has no dynamic-viewport-state precedent to use instead).
- `brdfLut_` (new `VulkanBRDFLut` class, a 512×512 2D texture, not a
  cubemap) baked via `shaders/brdfLUT.frag` - a pure function of
  `(NdotV, roughness)`, needing no descriptor set or push constant at
  all, unlike every other bake in this codebase.
- `IBLDescriptor` (new, 3 bindings: irradiance/prefiltered/BRDF LUT)
  replaces the single-binding `irradianceDescriptor_` as the main
  pipeline's set 1.
- `triangle.frag` gained the specular IBL term and upgraded its ambient
  Fresnel to the roughness-aware `fresnelSchlickRoughness` (used by both
  the diffuse and specular halves) - a refinement `TECHNICAL_NOTES.md`
  §34 had already predicted, not scope creep.
- Real bake cost, not hand-waved: prefilter ≈134.1M texture samples,
  BRDF LUT ≈268M ALU-only iterations, on top of §34's 97.5M irradiance
  samples - a real, multi-hundred-millisecond one-time startup cost,
  flagged explicitly rather than assumed negligible.

Not yet done: re-baking on a live light-direction change - the skybox,
irradiance, and specular prefilter are all baked once from whatever
`lightDirection_` is at startup (the BRDF LUT itself doesn't depend on
light direction at all, so this doesn't apply to it).

---

## Phase 16 — Live-Resized Viewport Target

**Status: Complete**

Closes the "Live-resized viewport target" item Phase 11 had carried
since the dockable-editor-UI work: the offscreen scene target
(`sceneColorTarget_`) was fixed at 1280×720, so resizing the docked
"Viewport" panel just stretched the existing image rather than
re-rendering at the panel's actual pixel size (see `TECHNICAL_NOTES.md`
§36 for the full design).

- `VulkanSceneColorTarget::create()` takes `width`/`height` as required
  runtime parameters instead of baked-in `1280`/`720` constants.
- `Camera::getProjectionMatrix()` takes `aspectRatio` as a required
  parameter (removing the `Camera::ASPECT_RATIO` compile-time constant);
  every call site recomputes it fresh each frame from
  `sceneColorTarget().extent()` - the same "recompute, don't cache"
  discipline already applied to frustum planes and `lightViewProj()`.
  The screen-space LOD threshold's projection scale (Phase 14) picked up
  the same fix, since it also depended on the target's height.
- `VulkanContext::resizeSceneTarget(width, height)` destroys and
  recreates `sceneFramebuffer_`/`pipeline_`/`skyboxPipeline_`/
  `sceneColorDepth_`/`sceneColorTarget_` at the new size (`pipeline_`/
  `skyboxPipeline_` need it since every pipeline in this codebase bakes
  a static viewport at creation time, confirmed no dynamic-viewport-
  state precedent exists to use instead); `sceneRenderPass_` itself is
  untouched (a `VkRenderPass` doesn't encode extent). Blocks on a full
  `vkDeviceWaitIdle`, not a per-frame-slot fence wait, since these
  resources are shared across both frames-in-flight, not per-slot.
- Detected in `ImGuiPass` (comparing `ImGui::GetContentRegionAvail()`
  against the current extent) but *applied* at the top of the *next*
  frame in `FrameRenderer::drawFrame()`, not immediately - by the time
  `ImGuiPass` runs, `GeometryPass` has already recorded draws this frame
  against the current target.
- The ImGui-registered "Viewport" texture (`sceneViewportSet_`) is
  re-registered (`ImGui_ImplVulkan_RemoveTexture`/`AddTexture`) right
  after a resize, since it was bound to the now-destroyed old
  `VkImageView`.

**Known, accepted limitation:** no debounce - while the panel border is
actively being dragged, this can trigger a `vkDeviceWaitIdle` stall on
consecutive frames, a real but accepted stutter traded for keeping the
resize path simple, matching this project's existing "simplest correct
implementation" bar elsewhere. IBL's baked-once assets (Phase 15) are
unaffected by viewport size and don't need any re-bake.

---

## Phase 17 — Live-Resized Window / Swapchain

**Status: Complete**

Closes the gap Phase 11 flagged and Phase 16 only partially closed: "This
codebase has no swapchain resize handling anywhere else either" was true
of the *window itself*, not just the docked Viewport panel Phase 16
fixed - the GLFW window was hard-coded `GLFW_RESIZABLE = GLFW_FALSE`
from Phase 0 through Phase 16, so dragging the OS window border was
never actually possible until now. See `TECHNICAL_NOTES.md` §39 for the
full design.

- `Application::init()` - the window is now created with
  `GLFW_RESIZABLE = GLFW_TRUE`.
- `VulkanContext::resizeSwapchain()` - destroys and recreates
  `framebuffer_`/`depthBuffer_`/`swapchain_` at the window's current
  framebuffer size. `renderPass_` is untouched (a `VkRenderPass` doesn't
  encode extent - the same fact Phase 16 already relied on for
  `sceneRenderPass_`); `pipeline_`/`skyboxPipeline_` need no changes at
  all, since Phase 11 already moved both off the swapchain onto the
  offscreen `sceneRenderPass_`/`sceneColorTarget_` - this resize doesn't
  touch either. Blocks on a full `vkDeviceWaitIdle`, same reasoning as
  `resizeSceneTarget()`.
- `FrameRenderer::drawFrame()` detects a resize two ways: comparing
  `glfwGetFramebufferSize()` against the swapchain's current extent at
  the top of every frame (an ordinary window drag), and
  `vkAcquireNextImageKHR`/`vkQueuePresentKHR` themselves returning
  `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` (a swapchain gone stale
  for a reason other than a plain resize, e.g. a display mode change).
  `frame.inFlightFence` is now reset only *after* a successful acquire,
  not before - resetting it first and then bailing out on an out-of-date
  acquire would leave the fence signaled-never, hanging the next frame
  that waits on the same slot.
- `FrameRenderer::recreateSwapchainResources()` - re-sizes
  `imagesInFlight`/`imageRenderFinished` to the (usually unchanged)
  swapchain image count and calls `ImGui_ImplVulkan_SetMinImageCount()`,
  since ImGui's Vulkan backend needs to know if that count ever changes.
- `Application::mainLoop()` pauses (via `glfwWaitEvents()`) whenever the
  framebuffer reports 0×0 (window minimized) instead of calling
  `drawFrame()` at all - a 0-sized swapchain is invalid to create, and
  there's nothing to render to it anyway.

**Known, accepted limitation:** no debounce, same tradeoff Phase 16
already accepted for the viewport panel - dragging the window border
can trigger a `vkDeviceWaitIdle` stall on consecutive frames.

---

## Phase 18 — Mesh-Detail-Derived LOD2 Threshold

**Status: Complete**

Closes the LOD gap this document had carried since Phase 12/14:
`lod1ScreenSize_`/`lod2ScreenSize_` were two *independent* hand-picked
constants (120px/60px), with nothing tying either to how detailed the
LOD1/LOD2 meshes actually are relative to each other. There's only one
object type in the scene (343 copies of the same Suzanne at 3 LOD
levels), so "per-mesh detail" here means grounding the threshold in the
actual triangle-count drop-off between the LOD meshes themselves, not
differentiating between object types. See `TECHNICAL_NOTES.md` §40 for
the full design.

- `VulkanContext::initSceneData()` captures each LOD's triangle count
  right after that LOD's own `ObjLoader::load()` call.
- `lod2ScreenSize_`'s startup default is now `lod1ScreenSize_ *
  lod2DetailRatio()` (the LOD2/LOD1 triangle-count ratio) instead of an
  independent literal - measured at ~19.5px for the current assets
  (968/289/47 triangles for LOD0/1/2), down from the old flat 60px.
  `lod1ScreenSize_` stays the one manually-anchored top-level threshold
  - there's no "LOD -1" mesh to derive it from.
- The "GPU Culling Stats" ImGui window shows the triangle counts/ratio
  and a "Reset LOD2 to mesh-derived default" button
  (`resetLod2ScreenSizeToMeshDefault()`); the sliders themselves are
  unchanged, still live-tunable.
- No `culling.comp`/`ComputeDescriptor`/`FrustumPlanes` changes - purely
  a CPU-side default-derivation and ImGui-display change.

---

## Phase 19 — Swept Projectile Collision

**Status: Complete**

Closes the discrete-collision tradeoff §20 accepted as theoretical and
§37 flagged as newly reachable once live-resize gave the frame loop a
real stall source: the projectile's grid-impact test only checked its
position once per frame, so a large enough `deltaTime`/speed could move
it further in one frame than the hit radius, tunneling through an
instance without the point check ever registering inside it. See
`TECHNICAL_NOTES.md` §41 for the full design and a reproduced, then
fixed, tunneling case.

- `Projectile::previousPosition()` — the pre-move position, captured at
  the top of `update()` before integrating this frame's movement.
- `VulkanContext::updateInstanceSimulation()`'s projectile-vs-grid check
  now sweeps the segment `[previousPosition(), position()]` against every
  instance's collision sphere (closest-point-on-segment test), instead of
  testing only the post-move point. Finds the *earliest* hit along the
  segment (smallest `t`), not just the first instance index that happens
  to overlap it, and applies the blast from that actual impact point
  rather than wherever the projectile ended up this frame.
- Verified by temporarily launching a 300 u/s projectile straight through
  a column of instance centers with one artificially large (~250ms)
  `deltaTime` frame: the old point check would have missed entirely
  (confirmed via a temporary diagnostic comparison, `segLen≈79` vs. a
  ~1.3-1.8 hit radius), while the new swept check caught it mid-segment
  (`t≈0.52`). No change in normal-speed behavior — the swept test reduces
  to the same result as the old point check whenever a frame's movement
  is small relative to the hit radius, which is every frame at default
  settings.
- No `culling.comp`/`ComputeDescriptor`/GPU-side changes at all — this
  entire fix is CPU-side, inside `updateInstanceSimulation()`'s existing
  per-frame simulation step.

---

## Phase 20 — Real PBR Material (ambientCG)

**Status: Complete**

Closes the "placeholder PBR texture maps" gap from Phase 8 milestone 2:
`normal.png`/`metallic_roughness.png`/`ao.png` were small self-generated
flat/gradient PNGs, good enough to validate the sampling mechanism but
not real material photography. See `docs/TECHNICAL_NOTES.md` §42 for
the full sourcing and channel-combination process.

- Sourced [ambientCG](https://ambientcg.com)'s `Bricks097` material
  (CC0 1.0 Universal, public domain) — real photogrammetry Color/
  NormalGL/Roughness/AmbientOcclusion maps, resized to 512×512.
- `metallic_roughness.png` combines the real Roughness map (G channel)
  with a constant-0 metalness (B channel) — physically correct for a
  non-metal brick material, not a fabricated value; brick materials
  don't ship a Metalness map at all since it would just be uniformly 0.
- `test_texture.png` (the albedo, previously a synthetic checker test
  pattern, not itself named in the roadmap gap) was swapped to the same
  material's real Color map too — a deliberate scope addition beyond
  the literal Phase 8 gap, since pairing real normal/roughness/AO data
  with an unrelated synthetic checker albedo would look visually
  incoherent.
- Zero code changes anywhere - `VulkanTexture::create()`'s format split,
  `VulkanDescriptor`'s bindings, and `triangle.frag`'s sampling logic are
  all untouched; this is a pure asset-content swap under the same 4
  filenames `VulkanContext::initCore()` already loads.
- Verified by rebuilding and confirming `[Texture] loaded assets/*.png
  (512x512)` for all 4 files with no load errors, then a visual check of
  the running app (no crash, no black/NaN artifacts - the shading
  pipeline's degenerate-UV guard from §25 is unaffected since only LOD0,
  which has real UV data, samples these maps in a way that exercises
  tangent reconstruction).

---

## Phase 21 — Transparency: Alpha Blending + GPU Sort

**Status: Complete**

Lays the groundwork for planned future materials with real transparency
(jelly, glass, liquid) — see `docs/TECHNICAL_NOTES.md` §43 for the full
design. The graphics pipeline had `blendEnable = VK_FALSE` everywhere,
and the GPU-driven culling pass compacts visible instances via
`atomicAdd`, so draw order is whichever thread finishes first — fine for
opaque (depth test alone resolves occlusion), wrong for alpha blending
(overlapping transparent instances would composite in undefined order).

- `culling.comp` now writes camera distance (already computed for the
  LOD test) into each compacted instance's otherwise-unused
  `position.w` — `triangle.vert` never reads it, so this costs nothing.
- New `sortInstances.comp`, dispatched `(3,1,1)` (one workgroup per LOD
  bucket): loads each bucket into shared memory and runs a parallel
  odd-even transposition sort, descending by that distance — farthest
  first, the order back-to-front blending needs. Chosen over a bitonic
  network for simplicity (no power-of-2 partner-index math) at a scale
  (≤343 elements) where the extra O(n²) phases cost microseconds either
  way. Shares `computeDescriptor_`'s existing bindings — no new buffers.
- `GeometryPass` reverses the LOD bucket draw order (`2,1,0` instead of
  `0,1,2`) once transparent — provably correct macro-ordering between
  buckets, since LOD0's camera-distance range is strictly less than
  LOD1's, which is strictly less than LOD2's, by the existing
  `lod1ScreenSize_ >= lod2ScreenSize_` invariant. The sort only needs to
  fix ordering *within* each bucket.
- New `VulkanContext::transparentPipeline_` (blend on, depth-write off,
  depth-test still on) — a sibling of `pipeline_`, since this codebase
  bakes pipeline state at creation with no dynamic-blend precedent.
  `VulkanContext::gridAlpha()` (default 1.0, byte-identical to prior
  behavior) selects which pipeline `GeometryPass` binds.
- `triangle.frag` now outputs `material.albedo.a` instead of a hardcoded
  `1.0` — the push constant's alpha channel was already there, just
  unused until now.
- "GPU Culling Stats" ImGui window gained a "Transparency" section: a
  "Grid Alpha" slider and an "Enable Transparency Sort" checkbox (default
  on) that demonstrates the blending-order bug it fixes when toggled off.
- Verified by temporarily forcing `gridAlpha_ < 1.0` and screenshotting:
  confirmed correct translucent rendering with the sort on, and no
  crash/corruption with it off (the bucket-level ordering alone already
  gives a reasonable approximation from typical camera angles, which is
  why the within-bucket sort's effect is subtle in a static screenshot —
  expected, not a sign it's not working).

**Explicitly out of scope, left for follow-up work:**
- The actual jelly/glass/liquid **shading model** (refraction sampling
  of the offscreen scene target, IOR, fake subsurface/wrap lighting).
- Mixed opaque+transparent instances in the same scene — `gridAlpha()`
  is one shared toggle for the whole material, matching the existing
  single-shared-`Material` architecture.
- The projectile's blend order relative to the grid isn't sorted (it's
  always drawn last) — a known, accepted limitation, not addressed here.
- Order-independent transparency (weighted blended OIT) as an
  alternative to sorting — considered, not pursued since sorting was
  the requested direction.

---

## Phase 22 — Master Texture Toggle

**Status: Complete**

Adds a runtime on/off switch for material texture sampling, default
**off** — a deliberate, visible default change, not a no-op one. See
`docs/TECHNICAL_NOTES.md` §44 for the full rationale.

- `MaterialPushConstants::metallicRoughness.z` (previously unused) is
  now `1.0` (use texture maps) or `0.0` (flat push-constant-only
  shading) — set from `VulkanContext::texturesEnabled()` for both the
  grid and projectile's push constants.
- `triangle.frag` branches on it (uniform per-draw, so branching around
  the `texture()` calls is safe/uniform control flow): off skips
  `texSampler`/`normalMap`/`metallicRoughnessMap`/`aoMap` sampling
  entirely, reproducing Phase 8 milestone 1's flat PBR look (real
  lighting/shadows/IBL, no material texture detail) rather than a
  fabricated "no texture" placeholder.
- New "Material" section in the "GPU Culling Stats" ImGui window: an
  "Enable Textures" checkbox.
- Verified both states: off (default) renders flat/untextured, matching
  Phase 8 milestone 1's look; on reproduces the real brick material
  (Phase 20) pixel-for-pixel as before this toggle existed.

---

## Phase 23 — Transparency Shading Model

**Status: Milestones 1-2 complete; M3 planned, not yet implemented**

Direct follow-up to Phase 21, which built the alpha-blend/GPU-sort
infrastructure but explicitly scoped out the real transparent shading
model, mixed-material scenes, and projectile sort ordering.

**Milestone 1 — Refraction/IOR shading (glass/jelly/liquid) — Complete**

Real screen-space refraction, global toggle (resolved the "global vs.
per-instance" open question below in favor of global — ships
independently of M2, which per-instance would have required as a
prerequisite). See `docs/TECHNICAL_NOTES.md` §45 for the full design.

Implemented, deliberately **not** as originally planned: the original
design below called for splitting `GeometryPass` into two render pass
instances (opaque draws → copy → transparent draws, same-frame). Instead,
shipped a lower-risk alternative that reaches the same visual result:
**capture the previous frame's fully-composited `sceneColorTarget_`** into
a new `sceneColorCopy_` once per frame, *before* this frame's own scene
render pass begins — refractive draws sample last frame's result (one
frame of temporal lag, imperceptible at normal camera speed/60fps). This
avoids any `FrameGraph`/`GeometryPass`-structure change at all, which the
original plan had flagged as the single biggest-risk piece.

- `VulkanSceneColorTarget` gained a `sampler()` (every image-owning class
  in this codebase owns its sampler) and unconditional
  `TRANSFER_SRC_BIT | TRANSFER_DST_BIT` usage, so the same class serves as
  both the copy source (`sceneColorTarget_`) and destination
  (`sceneColorCopy_`).
- `VulkanPipeline::create()` gained two defaulted parameters
  (`refractionLayout`, `fragShaderPath`) — `pipeline_`/
  `transparentPipeline_`'s existing call sites needed zero changes.
- New `shaders/triangle_refractive.frag` (a deliberate fork of
  `triangle.frag`, not a branch inside it — see §45 for why a shared
  shader with pipelines on different descriptor-set-count layouts isn't
  possible) samples `sceneColorCopy_` via a new set-2 descriptor
  (`refractionDescriptor_`, reusing `CubeSamplerDescriptor`'s 1-binding
  shape for a flat 2D image — Vulkan's descriptor plumbing doesn't care
  about view dimensionality, only the shader's declared sampler type).
- `refractivePipeline_` uses **opaque** depth behavior (blend off, depth
  write on), not `transparentPipeline_`'s — refraction composites in-shader
  rather than via fixed-function blending, so ordinary depth test+write
  already gives correct inter-instance occlusion with no sort needed.
- IOR stored in `MaterialPushConstants.metallicRoughness.w` (previously
  unused) — same "reuse an idle struct slot" trick §43 used for
  `position.w`. `SceneData.shadowParams.y/z` (previously unused) carry
  `sceneColorTarget_`'s pixel width/height, since the fragment shader has
  no view/projection matrix to derive its own screen UV from.
- Refraction offset is a hand-tuned-constant approximation (deviation of
  the refracted ray from the straight-through view ray, projected onto a
  local screen-tangent basis), not an exact reprojection — same
  small-angle-approximation bar as §14's screen-space LOD math.
- ImGui: "Enable Refraction (glass/jelly)" checkbox + "IOR" slider in the
  existing Transparency section. No separate "Refraction Strength"
  slider shipped — folded into one hand-tuned shader constant instead,
  to keep the single spare push-constant float sufficient.
- Verified interactively: default (off) is byte-identical to pre-M1
  behavior by construction (the whole copy/pipeline path is unreached);
  enabled, shows visible IOR-proportional background distortion through
  the grid, no validation errors, no crash.

**Deferred from the original M1 scope, left for later:** a real
reprojected (not hand-tuned) refraction offset; a "Refraction Strength"
slider; wrap-lighting fake-subsurface approximation for jelly.

**Milestone 2 — Mixed opaque + transparent instances — Complete**

Real GPU-side bucketing, implemented as originally planned (not a
per-fragment `discard` shortcut - see `docs/TECHNICAL_NOTES.md` §46 for
why that alternative was rejected). `gridAlpha()`/`refractionEnabled()`
used to be one shared toggle for the whole grid; now every
`materialStride()`'th instance (default every 3rd, ImGui-tunable) can use
whichever special material those toggles currently select, while the
rest of the grid stays on the always-opaque `pipeline_` regardless.

- `ObjectData` (both `culling.comp` and its C++ mirror) gained
  `materialFlags.x` - CPU-computed every frame from
  `mixedMaterialsEnabled()`/`materialStride()`, same "recompute, don't
  cache" discipline `boundingSphere` already used.
- `culling.comp` compacts each LOD tier into one of *two* buffer pairs
  (normal vs. special) depending on that flag, instead of always one -
  same atomic-compaction bucketing pattern Phase 6's LOD tiers and Phase
  21's sort buckets already established. `ComputeDescriptor` grew
  14→20 bindings; `sortInstances.comp` grew from 3 workgroups to 6
  (dispatched unconditionally - sorting an empty/opaque bucket is a fast
  near-no-op, cheaper than a second conditional dispatch).
- `GeometryPass` factored its bind/push/draw sequence into a
  `drawGridBucket` helper (shared by both buckets instead of duplicated),
  called once for the normal bucket and, only when mixed materials are
  on, a second time for the special bucket with whichever pipeline
  `gridAlpha()`/`refractionEnabled()` select.
- Default off - the special buffers are guaranteed empty by construction
  whenever `mixedMaterialsEnabled()` is false, so the feature is a true
  no-op until explicitly turned on.
- ImGui: "Enable Mixed Materials" checkbox + "Special Material Every N"
  slider (1-20) in the existing Transparency section.
- Verified interactively: combined with both Grid Alpha < 1 and
  Refraction, every Nth instance visibly takes the special material while
  the rest stay opaque brick; toggled off, indistinguishable from pre-M2
  behavior.

**Milestone 3 — Projectile transparent sort/ordering**

The projectile always draws last regardless of blend correctness.
Planned approach: keep the projectile's separate UBO/descriptor
(deliberate since Phase 7 — not folding it into the grid's instance
buffer), but have the CPU compare its per-frame camera distance against
the LOD buckets' known distance ranges to decide which of the reversed
`2,1,0` bucket-draw slots it should be inserted into, rather than always
drawing after all three.

---

## Open / not yet started

- **LOD1/LOD2 have no real UV data** (Phase 8, milestone 2) — only LOD0
  was swapped to a UV/normal-mapped mesh; the far LOD meshes sample a
  constant `(0,0)` texel, same flat-tinted look as before this milestone.
  Closing this needed a UV-preserving decimation of the base mesh (a 3D
  tool this environment doesn't have — see `TECHNICAL_NOTES.md` §25).
  **Update (2026-09-04):** UV-mapped replacements uploaded —
  `assets/suzanne_lod1_uv.obj` (163v/215vt/289tri) and
  `assets/suzanne_lod2_uv.obj` (47v/80vt/76tri), made externally in
  Blender from `suzanne_pbr.obj`. Checked against `ObjLoader.cpp`
  (tinyobj-based, auto-triangulates, ignores `.mtl` entirely since
  materials come from the separate `Material` class): both files are
  well-formed (complete `v/vt/vn` triplets, UVs in `[0,1]`) and load
  cleanly. LOD2's triangle count (76) differs from the prior non-UV
  LOD2 (47), which needs no code change since `lod2DetailRatio()`
  already derives its default from the live post-load triangle count
  rather than a hardcoded constant (Phase 18). Wiring is a two-line
  swap in `VulkanContext.cpp` (~L763-764) — verified but not yet
  applied.

---

## Long-term goals

- Modern Vulkan architecture (in progress, core patterns established)
- GPU-driven rendering research direction (demonstrated with working
  culling + LOD pipeline, now including hierarchical/multi-pass culling
  - Phase 13; neural-rendering hybrid approaches still under
  consideration for graduate study direction)
- PBR material model, approached incrementally: interactive objects
  (Phase 7) first, then lighting math (Phase 8 milestone 1), then
  texture-based materials (Phase 8 milestone 2), then full image-based
  ambient lighting (Phase 15, complete) — a real `Material` class exists
  now, but per-object material variation (beyond the existing
  push-constant factors) is still future work, not a jump straight to a
  full material system
- Foundation for future rendering experiments

