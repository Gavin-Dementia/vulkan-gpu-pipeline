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
- Mutual instance-vs-instance collision (positional pushout, every
  frame) — fixes scattered instances visually clipping through each
  other once settled; see `TECHNICAL_NOTES.md` §21
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
  regression, just not textured. Also placeholder textures throughout
  (`assets/normal.png`, `metallic_roughness.png`, `ao.png` are small
  self-generated PNGs — flat normal, a metallic/roughness gradient, flat
  AO — not sourced/authored PBR photo sets), same "good enough now,
  refine later" bar this project already applies elsewhere (LOD
  thresholds, the flat ambient term).

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
- `ShadowPass` draws all 343 grid instances unculled (no light-frustum
  culling needed at this instance count) by binding `objectBuffer_`
  directly as the instance buffer — no new buffer, since it already
  holds `vec4(position, radius)` per instance every frame, byte-identical
  to `InstanceData`'s layout
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

Not yet done: light-frustum culling for the shadow pass (draws all
instances unculled every frame — fine at 343 instances); `lightViewProj()`'s
`kSceneRadius` is a fixed constant, not re-derived from the live scatter
state, so an instance blasted far enough away could stop casting a
shadow.

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
  1280×1024 — matches `Camera::ASPECT_RATIO`'s existing fixed-window
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

Not yet done: thresholds are still a flat distance in world units, not
derived from mesh screen-space size or per-mesh detail level - still a
manually-tuned pair of numbers, just no longer ones that need a shader
recompile to change.

---

## Open / not yet started

- **Live-resized viewport target** (Phase 11) — the offscreen scene
  texture is fixed-resolution; resizing the "Viewport" panel scales the
  existing image rather than re-rendering at a new resolution. Would need
  swapchain-style resize handling this project doesn't have anywhere else.
- **LOD1/LOD2 have no real UV data** (Phase 8, milestone 2) — only LOD0
  was swapped to a UV/normal-mapped mesh; the far LOD meshes sample a
  constant `(0,0)` texel, same flat-tinted look as before this milestone.
- **Placeholder PBR texture maps, not sourced/authored ones** (Phase 8,
  milestone 2) — `normal.png`/`metallic_roughness.png`/`ao.png` are small
  self-generated flat/gradient PNGs, good enough to validate the sampling
  mechanism, not real material photography.
- **Swept (not discrete) projectile collision** — current hit test only
  checks position once per frame; not observable at the current
  speed/instance-radius ratio, but would need revisiting for a much
  faster projectile or much smaller instances (§20)
- **Velocity-based mutual collision response** — current fix (§21) is a
  positional pushout only (no bounce/restitution); instances stop dead
  on contact with each other rather than deflecting, fine for "no
  clipping" but not a physically realistic collision
- **IBL / environment lighting** — current ambient term is a flat
  `0.03 * albedo` constant (shadow mapping for the direct term is now
  implemented — see Phase 9)
- **Texture sampling reunited with the primary mesh** — implemented and
  validated in isolation (§13), but the Suzanne LOD chain still has no
  texcoord data on any of its 3 variants
- **Multi-pass / hierarchical culling** — current design is a flat
  343-thread scan; fine at this scale, would need a coarser first pass
  at much higher instance counts
- **LOD thresholds are still a flat world-space distance** (Phase 12
  made them runtime-tunable, not shader constants, but they're still a
  manually-set pair of numbers, not derived from mesh screen-space size
  or per-mesh detail level)

---

## Long-term goals

- Modern Vulkan architecture (in progress, core patterns established)
- GPU-driven rendering research direction (demonstrated with working
  culling + LOD pipeline; candidate next steps: hierarchical/multi-pass
  culling, neural-rendering hybrid approaches under consideration for
  graduate study direction)
- PBR material model, approached incrementally: interactive objects
  (Phase 7) first, then lighting math (Phase 8 milestone 1), then
  texture-based materials (Phase 8 milestone 2, both complete) — a real
  `Material` class exists now, but per-object material variation (beyond
  the existing push-constant factors) is still future work, not a
  jump straight to a full material system
- Foundation for future rendering experiments

