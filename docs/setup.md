# Setup Guide

Detailed setup and build instructions for Vulkan GPU Pipeline.

---

## 1. System requirements

| Requirement | Minimum |
|---|---|
| OS | Windows 10 / 11 |
| CMake | 3.20 |
| Compiler | MSVC 2022 (recommended) or MinGW-w64 |
| GPU | Vulkan 1.2 capable |

---

## 2. Vulkan SDK

Download: https://vulkan.lunarg.com/sdk/home
Tested version: `1.4.350.0`

During installation, select:
- GLM headers
- Debug shader toolchain libraries (`glslc` is required for shader compilation)
- VMA header

After install, verify the environment:

```bash
vulkaninfo
glslc --version
```

If `vulkaninfo` fails, the `VULKAN_SDK` environment variable is likely not set.
Re-run the Vulkan SDK installer and ensure "Add to PATH" is checked.

---

## 3. Clone the repository

```bash
git clone --recursive https://github.com/Gavin-Dementia/vulkan-gpu-pipeline.git
cd vulkan-gpu-pipeline
```

If already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

This pulls GLFW and GLM into `third_party/`.

---

## 4. Build

```bash
cmake -S . -B build
cmake --build build
```

What happens during build:
- GLFW is compiled from source (submodule)
- Shaders in `shaders/` are compiled to SPIR-V via `glslc` and placed in `build/bin/shaders/compiled/`
- Assets in `assets/` are copied to `build/bin/assets/`
- Executable is placed at `build/bin/app.exe` (MSVC's multi-config generator
  actually places it at `build/bin/Debug/app.exe`; a Visual Studio + CMake-presets
  workflow may instead output to `out/build/<preset>/bin/app.exe`)

To rebuild shaders after editing `.vert` or `.frag` files, re-run `cmake --build build`.

---

## 5. Project structure

```
vulkan-gpu-pipeline/
├── assets/                    OBJ meshes, textures
├── include/
│   ├── core/                  Application, Camera, Projectile
│   ├── ui/                    ImGuiLayer
│   └── vulkan/
│       ├── buffer/            VulkanBuffer, VertexBuffer, IndexBuffer,
│       │                      UniformBuffer, IndirectDrawBuffer
│       ├── command/           VulkanCommandPool
│       ├── core/              VulkanInstance
│       ├── culling/           Frustum (Gribb-Hartmann plane extraction)
│       ├── descriptor/        VulkanDescriptor, ComputeDescriptor
│       ├── device/            VulkanDevice
│       ├── frame/             FrameContext, FrameRenderer, FrameGraph
│       ├── instance/          InstanceData (per-instance vertex attribute)
│       ├── lighting/          SceneData, MaterialPushConstants
│       ├── pipeline/          VulkanPipeline, VulkanComputePipeline
│       ├── platform/          VulkanSurface
│       ├── renderpass/        VulkanRenderPass, VulkanFramebuffer, VulkanDepthBuffer
│       ├── resource/          ShaderLoader, ObjLoader
│       ├── swapchain/         VulkanSwapchain
│       └── texture/           VulkanTexture
├── shaders/                   GLSL source (triangle.vert/frag, culling.comp)
├── src/                       Implementation (.cpp), mirrors include/
└── third_party/               GLFW, GLM, ImGui, stb, tinyobjloader
```

---

## 6. CMake design notes

- GLFW: compiled as submodule, no system install needed
- GLM: header-only interface library
- tinyobjloader: header-only interface library (`third_party/tinyobjloader/`)
- Vulkan: located via `find_package(Vulkan REQUIRED)` — requires Vulkan SDK on PATH
- Shaders: auto-compiled via `add_custom_command` using `glslc`
- Assets: auto-copied via `POST_BUILD` step

No external package manager (vcpkg, conan) required.

---

## 7. Common issues

**`find_package(Vulkan)` fails**
- Ensure Vulkan SDK is installed and `VULKAN_SDK` is in environment variables
- Re-open terminal after SDK install

**`glslc` not found**
- Ensure Vulkan SDK installed the shader toolchain component
- Check: `where glslc` on Windows

**Submodule directory empty**
- Run: `git submodule update --init --recursive`

**`WinMain` linker error**
- CMake is configured for `WIN32_EXECUTABLE OFF` (console subsystem) — do not change this

**Assets not found at runtime**
- The POST_BUILD step copies `assets/` to `build/bin/assets/`
- If missing, manually copy or re-run `cmake --build build`

---

## 8. FrameGraph design note

Passes are registered with explicit dependency lists:

```cpp
int geometryPass = graph->addPass({ "GeometryPass", {}, ... });
int lightingPass = graph->addPass({ "LightingPass", { geometryPass }, ... });
graph->addPass({ "PostProcess", { lightingPass }, ... });
graph->build();  // resolves topological order, detects cycles
```

`build()` uses Kahn's algorithm to sort passes. A cycle throws
`std::runtime_error("FrameGraph has cycle!")` before the first frame.

New passes (e.g. a Compute culling pass) can be inserted by adding a node
to the graph with the appropriate dependencies — no changes to `FrameRenderer`
or `drawFrame()` required.

