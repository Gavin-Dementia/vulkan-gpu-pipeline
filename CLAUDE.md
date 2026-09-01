# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

A GPU-driven Vulkan renderer built from scratch in C++17, centered on a `FrameGraph` DAG (Kahn's algorithm) that separates Compute and Graphics passes. The concrete feature under active development is compute-shader frustum culling + distance-based LOD selection via GPU-side atomic compaction into indirect draw buffers — i.e. the CPU submits `vkCmdDrawIndexedIndirect` without ever reading back which instances are visible or how many.

## Build & run

```bash
cmake -S . -B build
cmake --build build
```

- Requires the Vulkan SDK on `PATH` (`vulkaninfo` and `glslc --version` should both work) and CMake ≥ 3.20.
- Uses git submodules for GLFW and GLM (`third_party/glfw`, `third_party/glm`) — clone with `--recursive` or run `git submodule update --init --recursive`.
- Shaders (`shaders/*.vert|.frag|.comp`) are auto-compiled to SPIR-V via `glslc` as a CMake custom target (`Shaders`), and `assets/` is auto-copied to the output dir as a `POST_BUILD` step. Both happen automatically on every `cmake --build build` — no separate asset/shader step needed.
- The MSVC multi-config generator puts the executable at `build/bin/Debug/app.exe` (a Visual Studio + CMake-presets workflow may instead output to `out/build/<preset>/bin/app.exe`).
- `CMakeLists.txt` uses **explicit source file lists** (no globbing) grouped into `set(...)` variables per subsystem (`VULKAN_BUFFER_SRC`, `VULKAN_FRAME_SRC`, etc.) — adding a new `.cpp` file requires adding it to the relevant list, not just creating the file.
- No lint config and no automated test suite exist in this repo. Verification is: does it build cleanly, and does the running app look correct (culling/LOD behavior is inherently visual — see `docs/TECHNICAL_NOTES.md` for the project's own debugging methodology for this, e.g. §11 on GPU readback timing and the `sin(time)` oscillation test used to validate culling logic experimentally rather than by inspection).
- Console output on a correct run includes lines like `[ObjLoader] 507 unique vertices, 2904 indices (from assets/suzanne.obj), recentered by (...), bounding radius ...` for each of the 3 LOD meshes, followed by `Vulkan Context initialized` / `[FrameRenderer] initialized` / `Application mainLoop`. Note: stdout is fully buffered when redirected to a file/pipe rather than a console, so a force-killed background run may show an empty captured log even on a successful start.

## Architecture

Read `docs/architecture.md` and `docs/TECHNICAL_NOTES.md` before making non-trivial changes to the rendering pipeline — the former is the current-state reference (updated in lockstep with the code), the latter is a decision log (what was tried, why, what broke) written specifically so a change's rationale doesn't have to be reverse-engineered later. `docs/roadmap.md` tracks what's implemented vs. planned per phase. Keep these three in sync with the code when architecture changes — this repo has previously drifted (see TECHNICAL_NOTES §15) and the drift itself had to be found and fixed.

High-level structure, in initialization/ownership order:

```
Application (owns the GLFW window, the main loop, deltaTime)
└── VulkanContext — split into 3 init phases specifically to avoid a god-function:
    ├── initCore()              — instance → device → swapchain → depth → renderpass → pipeline → framebuffer
    ├── initSceneData()         — loads 3 LOD meshes via ObjLoader, builds the 7×7×7 instance grid
    └── initCullingResources()  — shared object bounding-sphere buffer + 3 parallel per-LOD
                                   {visible-instance, indirect-draw} buffer pairs + compute descriptor/pipeline
        └── FrameRenderer — per-frame sync (fence/semaphore), owns the FrameGraph
            └── FrameGraph — DAG of passes; executeCompute() runs outside the render pass,
                              executeGraphics() runs inside it; GeometryPass declares GPUCullingPass
                              as a read dependency so the topological sort enforces ordering
```

Things that aren't obvious from any single file:

- **The culling pipeline is GPU-driven, not just GPU-accelerated.** The distinction (see TECHNICAL_NOTES §7) is atomic compaction: each compute thread that passes the frustum+LOD test calls `atomicAdd` on its LOD bucket's `instanceCount` to claim a unique output slot, so the GPU itself decides the final indirect-draw instance count — the CPU never reads culling results back before issuing `vkCmdDrawIndexedIndirect`. A design that computed visibility on the GPU but read it back on the CPU to decide the draw count would not qualify as GPU-driven by this project's own definition.
- **LOD is 3 fully parallel buffer sets, not a tag on one buffer.** Each LOD is a different mesh (different vertex/index buffer), and one indirect draw call can only bind one such pair — so `culling.comp` buckets each passing instance by camera distance into one of 3 independent `{VisibleLODN, IndirectLODN}` output pairs, and `GeometryPass` issues 3 `vkCmdDrawIndexedIndirect` calls per frame (see TECHNICAL_NOTES §15 for why this couldn't be a single buffer with a per-instance LOD field).
- **`Camera` is the single source of truth for both the view and projection matrix**, called identically from the culling compute pass (frustum construction) and the geometry pass (vertex transform). Never recompute the projection matrix inline elsewhere — that was previously duplicated in two places and consolidated into `Camera::getProjectionMatrix()`.
- **`ObjLoader::load()` recenters every mesh to its own bounding-box center** and computes `MeshData::boundingRadius` from the recentered geometry. This matters because the per-instance model matrix is a pure rotation with no translation compensation — an off-center mesh orbits its grid slot instead of spinning in place (see TECHNICAL_NOTES §16). The object-buffer bounding sphere used for culling comes from LOD0's `boundingRadius` (LOD1/2 are decimated versions of the same shape and are never larger).
- **`VulkanBuffer` persistently maps `HOST_VISIBLE` memory at creation** rather than mapping per `upload()`/`download()` call — several buffers (UBO, frustum, all 3 indirect-draw buffers) are touched every frame. This relies on every `HOST_VISIBLE` buffer in the codebase also being created `HOST_COHERENT`; don't add a `HOST_VISIBLE`-only buffer without also handling that.
- A `VkMemoryBarrier` between `executeCompute()` and `vkCmdBeginRenderPass` (`SHADER_WRITE` → `VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ`) is what makes the compute-write-then-graphics-read sequence actually safe — Vulkan does not implicitly order this across pipeline stages just because commands were recorded sequentially.
