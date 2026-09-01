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

Not yet done: LOD distance thresholds are hardcoded constants, not
derived from mesh detail or screen-space size; texture sampling still
isn't reunited with the (still UV-less) Suzanne LOD chain.

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
- Bundled in: manual **T** key pauses/resumes the grid's shared spin,
  useful for observing the scatter without the whole grid also rotating

---

## Phase 8 — PBR Lighting

**Status: Milestone 1 complete**

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

Milestone 2 (not started) — texture-based materials (albedo/normal/
metallic-roughness/AO maps) and a formal `Material` class, generalizing
the per-object-descriptor-set pattern this milestone already proved out.

---

## Open / not yet started

- **Texture-based PBR materials** (Phase 8, milestone 2) — albedo/
  normal/metallic-roughness/AO maps, a formal `Material` class
- **Swept (not discrete) projectile collision** — current hit test only
  checks position once per frame; not observable at the current
  speed/instance-radius ratio, but would need revisiting for a much
  faster projectile or much smaller instances (§20)
- **IBL / environment lighting** — current ambient term is a flat
  `0.03 * albedo` constant; no shadows either
- **Texture sampling reunited with the primary mesh** — implemented and
  validated in isolation (§13), but the Suzanne LOD chain still has no
  texcoord data on any of its 3 variants
- **Performance instrumentation** — GPU timestamp queries to quantify
  culling's (and now LOD selection's) actual frame-time impact
  (currently demonstrated functionally, not yet measured numerically)
- **Multi-pass / hierarchical culling** — current design is a flat
  343-thread scan; fine at this scale, would need a coarser first pass
  at much higher instance counts
- **LOD distance thresholds as data, not shader constants** — would let
  LOD ranges be tuned per mesh/scene without a shader recompile

---

## Long-term goals

- Modern Vulkan architecture (in progress, core patterns established)
- GPU-driven rendering research direction (demonstrated with working
  culling + LOD pipeline; candidate next steps: hierarchical/multi-pass
  culling, neural-rendering hybrid approaches under consideration for
  graduate study direction)
- PBR material model, approached incrementally: interactive objects
  (Phase 7) first, then lighting math (Phase 8 milestone 1, complete),
  texture-based materials next, rather than jumping straight to a full
  material system
- Foundation for future rendering experiments

