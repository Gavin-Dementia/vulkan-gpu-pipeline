# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

A GPU-driven Vulkan renderer built from scratch in C++17, centered on a `FrameGraph` DAG (Kahn's algorithm) that separates Compute, Shadow, and Graphics passes. The rendering core is compute-shader frustum culling + distance-based LOD selection via GPU-side atomic compaction into indirect draw buffers — the CPU submits `vkCmdDrawIndexedIndirect` without ever reading back which instances are visible or how many. On top of that core, the project has grown an interactive layer (a mouse-fired projectile that collides with and scatters a 343-instance grid), a PBR lighting direction (Cook-Torrance BRDF, currently push-constant materials with no textures yet), and shadow mapping for the single directional light (depth-only `ShadowPass`, 3×3 PCF, tunable bias — see `docs/roadmap.md` Phase 9 and `docs/TECHNICAL_NOTES.md` §22).

## Build & run

```bash
cmake -S . -B build
cmake --build build
```

- Requires the Vulkan SDK on `PATH` (`vulkaninfo` and `glslc --version` should both work) and CMake ≥ 3.20.
- Uses git submodules for GLFW and GLM (`third_party/glfw`, `third_party/glm`) — clone with `--recursive` or run `git submodule update --init --recursive`.
- Shaders (`shaders/*.vert|.frag|.comp`) are auto-compiled to SPIR-V via `glslc` as a CMake custom target (`Shaders`), and `assets/` is auto-copied to the output dir as a `POST_BUILD` step. Both happen automatically on every `cmake --build build` — no separate asset/shader step needed.
- The MSVC multi-config generator puts the executable at `build/bin/Debug/app.exe` (a Visual Studio + CMake-presets workflow may instead output to `out/build/<preset>/bin/app.exe`).
- `CMakeLists.txt` uses **explicit source file lists** (no globbing) grouped into `set(...)` variables per subsystem (`VULKAN_BUFFER_SRC`, `VULKAN_FRAME_SRC`, `CORE_SRC`, etc.) — adding a new `.cpp` file requires adding it to the relevant list, not just creating the file.
- No lint config and no automated test suite exist in this repo. Verification is: does it build cleanly, and does the running app look/behave correctly (culling/LOD/lighting/collision are all inherently visual — see `docs/TECHNICAL_NOTES.md` for this project's own debugging methodology, e.g. §11 on GPU readback timing and the `sin(time)` oscillation test used to validate culling logic experimentally rather than by inspection).
- Console output on a correct run includes lines like `[ObjLoader] 507 unique vertices, 2904 indices (from assets/suzanne.obj), recentered by (...), bounding radius ...` for each of the 3 LOD meshes, followed by `Vulkan Context initialized` / `[FrameRenderer] initialized` / `Application mainLoop`. Note: stdout is fully buffered when redirected to a file/pipe rather than a console, so a force-killed background run may show an empty captured log even on a successful start.

## Controls (the app is interactive, not just a passive demo)

WASD move, mouse-look (window captures the cursor by default), left-click fires the projectile at the grid, **R** resets the grid to its rest formation, **T** pauses/resumes the grid's spin, hold **Ctrl** to reveal the cursor and interact with the ImGui windows (stats overlay + live lighting/shadow-bias sliders + a shadow map debug preview), **Esc** quits.

## Architecture

Read `docs/architecture.md` and `docs/TECHNICAL_NOTES.md` before making non-trivial changes to the rendering pipeline — the former is the current-state reference (updated in lockstep with the code), the latter is a decision log (what was tried, why, what broke) written specifically so a change's rationale doesn't have to be reverse-engineered later. `docs/roadmap.md` tracks what's implemented vs. planned per phase. Keep these three in sync with the code when architecture changes — this repo has previously drifted (see TECHNICAL_NOTES §15) and the drift itself had to be found and fixed.

High-level structure, in initialization/ownership order:

```
Application (owns the GLFW window, the main loop, deltaTime, input polling)
└── VulkanContext — split into 3 init phases specifically to avoid a god-function:
    ├── initCore()              — instance → device → swapchain → depth → renderpass →
    │                              pipeline (+ a push-constant range for material params) →
    │                              framebuffer; also creates sceneDataBuffer_, the shadow
    │                              map's image/view/sampler (before both descriptor sets,
    │                              which bind it) plus its own depth-only render pass/
    │                              framebuffer/pipeline, and the grid's + projectile's
    │                              descriptor sets (both reference sceneDataBuffer_ + shadow map)
    ├── initSceneData()         — loads 3 LOD meshes via ObjLoader, builds the 7×7×7
    │                              instance grid, creates the projectile's 1-entry instance buffer
    └── initCullingResources()  — shared object bounding-sphere buffer (re-uploaded every
                                   frame, not write-once — see below) + 3 parallel per-LOD
                                   {visible-instance, indirect-draw} buffer pairs + compute
                                   descriptor/pipeline
        └── FrameRenderer — per-frame sync (fence/semaphore), owns the FrameGraph
            └── FrameGraph — DAG of passes across 3 stages (Compute/Shadow/Graphics);
                              executeCompute() and executeShadow() each run outside the main
                              render pass with their own explicit wrapper in drawFrame(),
                              executeGraphics() runs inside it; GeometryPass declares
                              GPUCullingPass + ShadowPass as read dependencies so the
                              topological sort documents the real ordering
```

Each frame, `Application::mainLoop()` does world-simulation work **before** calling `FrameRenderer::drawFrame()`: polls input, updates the projectile, runs `VulkanContext::updateInstanceSimulation(deltaTime)` (grid physics + collision, below), and updates the grid's spin angle. None of that lives inside a `FrameRenderer` pass lambda — it's not rendering state, and keeping it in `Application` avoids threading a `deltaTime` parameter through `drawFrame()`, which takes none.

Things that aren't obvious from any single file:

- **The culling pipeline is GPU-driven, not just GPU-accelerated.** The distinction (see TECHNICAL_NOTES §7) is atomic compaction: each compute thread that passes the frustum+LOD test calls `atomicAdd` on its LOD bucket's `instanceCount` to claim a unique output slot, so the GPU itself decides the final indirect-draw instance count — the CPU never reads culling results back before issuing `vkCmdDrawIndexedIndirect`. A design that computed visibility on the GPU but read it back on the CPU to decide the draw count would not qualify as GPU-driven by this project's own definition.
- **LOD is 3 fully parallel buffer sets, not a tag on one buffer.** Each LOD is a different mesh (different vertex/index buffer), and one indirect draw call can only bind one such pair — so `culling.comp` buckets each passing instance by camera distance into one of 3 independent `{VisibleLODN, IndirectLODN}` output pairs, and `GeometryPass` issues 3 `vkCmdDrawIndexedIndirect` calls per frame (see TECHNICAL_NOTES §15 for why this couldn't be a single buffer with a per-instance LOD field).
- **`objectBuffer_` is re-uploaded every frame, not write-once.** It started as a write-once startup upload; the grid collision/scatter system (TECHNICAL_NOTES §20) made it per-frame-dynamic with **zero compute shader or descriptor changes**, because `culling.comp` never had a concept of a "static" position in the first place — it just reads whatever's in the buffer at dispatch time. This is only affordable because `VulkanBuffer::upload()` is a persistently-mapped `memcpy` (see below), not a real per-frame cost.
- **Two different mechanisms carry per-object vs. per-frame-shared data, and mixing them up is a real bug, not a style choice.** `MaterialPushConstants` (albedo/metallic/roughness) is a push constant, re-issued via `vkCmdPushConstants` before *every* draw call, because it's pipeline state that persists across draws sharing a command buffer — the grid and the projectile must each re-push their own values or the second draw silently inherits the first's. `SceneData` (light + camera) is the opposite: a UBO shared via one buffer bound at binding 2 on *every* descriptor set, because it's identical for every draw this frame and duplicating it per-object would be wasteful and, worse, another opportunity to have two draws disagree about e.g. the camera position. See TECHNICAL_NOTES §17 and §19 for the two-descriptor-set / two-UBO story that led here (a single mutable UBO can't hold two different values for two draw calls in the same recorded command buffer — the GPU reads whichever value is in memory at execute time, not record time).
- **Render/culling bounding radius and collision radius are deliberately separate values** (`boundingSphereRadius_` vs. `collisionRadius_` on `VulkanContext`) even though they start equal — one must stay geometrically accurate (culling) and the other is a gameplay-feel parameter meant to be tuned independently (TECHNICAL_NOTES §20 addendum).
- **`Camera` is the single source of truth for view, projection, and forward direction**, used identically by the culling compute pass, the geometry pass, and `Projectile::launch()` (aims along `Camera::getForward()`). Never recompute the projection matrix inline elsewhere — that was previously duplicated in two places and consolidated into `Camera::getProjectionMatrix()`.
- **`ObjLoader::load()` recenters every mesh to its own bounding-box center** and computes `MeshData::boundingRadius` from the recentered geometry. This matters because the per-instance model matrix is a pure rotation with no translation compensation — an off-center mesh orbits its grid slot instead of spinning in place (see TECHNICAL_NOTES §16). The object-buffer bounding sphere used for culling comes from LOD0's `boundingRadius` (LOD1/2 are decimated versions of the same shape and are never larger).
- **`VulkanBuffer` persistently maps `HOST_VISIBLE` memory at creation** rather than mapping per `upload()`/`download()` call — several buffers (UBOs, frustum, all 3 indirect-draw buffers, `objectBuffer_`, `sceneDataBuffer_`) are touched every frame. This relies on every `HOST_VISIBLE` buffer in the codebase also being created `HOST_COHERENT`; don't add a `HOST_VISIBLE`-only buffer without also handling that.
- **The grid's rest formation (`cachedInstances_`) is kept alive for the app's lifetime**, not freed after init — it's the reference the **R**-key reset restores from. If you see code treating it as disposable scratch data, that's stale.
- A `VkMemoryBarrier` between `executeCompute()` and `vkCmdBeginRenderPass` (`SHADER_WRITE` → `VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ`) is what makes the compute-write-then-graphics-read sequence actually safe — Vulkan does not implicitly order this across pipeline stages just because commands were recorded sequentially.
- **A second render pass that transforms the same geometry needs the same per-draw transform applied a second time — nothing keeps two vertex shaders in sync automatically.** `ShadowPass` (`shadow.vert`) is an independent vertex transform of the grid's mesh, not a derivative of `GeometryPass`'s (`triangle.vert`); it originally omitted the grid's spin rotation entirely, so the shadow map was cast from each mesh's un-rotated rest pose while the visible geometry spun independently, drifting out of sync every frame. `ShadowPushConstants` now carries the same `model` matrix `GeometryPass` computes (`spinAngle()`'s rotation for the grid, identity for the projectile) — see TECHNICAL_NOTES §22.
