# Vulkan GPU Pipeline

A GPU-driven rendering framework built from scratch in C++17 and Vulkan,
designed around a **FrameGraph DAG** for explicit pass dependency and execution ordering.

---

## Demo

### GPU LOD Switching

![LOD Demo](docs/assets/lod_demo_01.gif)

### Screenshot

![LOD Screenshot](docs/assets/lod_demo_01.png)

---

## Features

| Feature | Status |
|---|---|
| Vulkan instance / device / swapchain | ✅ |
| RenderPass + Framebuffer | ✅ |
| FrameGraph (DAG, Kahn's algorithm) | ✅ |
| Vertex / Index Buffer | ✅ |
| OBJ mesh loading (tinyobjloader) | ✅ |
| Uniform Buffer (Camera / MVP) | ✅ |
| Texture Sampling | ✅ |
| Depth Buffer | ✅ |
| GPU Frustum Culling (Compute Shader) | ✅ |
| GPU LOD Selection | ✅ |
| Multi-LOD Mesh Rendering | ✅ |
| Indirect Draw Buffer | ✅ |

---

## Architecture

```
Application
└── VulkanContext          device, swapchain, renderpass, pipeline
    └── FrameRenderer      per-frame sync, command recording
        └── FrameGraph     DAG of render passes
            ├── Compute Pass
            │      ├── Frustum Culling
            │      └── LOD Selection
            ├── Graphics Pass
            │      └── Indirect Draw
            ├── GeometryPass   →  bind VB, draw
            ├── LightingPass   → (depends on Geometry)
            └── PostProcess    → (depends on Lighting)
```

The FrameGraph resolves pass execution order via **Kahn's algorithm** at build time,
catching dependency cycles before the first frame runs.

---

## Requirements

### 1. Vulkan SDK

Download: https://vulkan.lunarg.com/sdk/home
Tested version: `1.4.350.0`

Install with:
- GLM headers
- Debug shader toolchain
- VMA header

Verify:
```bash
vulkaninfo
```

### 2. C++ Compiler

**Windows (recommended)**
- Visual Studio 2022 with "Desktop development with C++"
- Windows 10/11 SDK

**MinGW** (experimental, not actively tested)

### 3. CMake

Version ≥ 3.20

```bash
cmake --version
```

---

## Clone

This repo uses **git submodules** (GLFW, GLM).

```bash
git clone --recursive https://github.com/Gavin-Dementia/vulkan-gpu-pipeline.git
```

If already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

---

## Build

```bash
cmake -S . -B build
cmake --build build
```

Executable: `build/bin/app.exe`

Shaders are compiled automatically via `glslc` during the build step.
Assets are copied to `build/bin/assets/` automatically.

---

## Dependencies

This project vendors all third-party libraries inside the `third_party/` directory.

### Third-party Directory

```text
third_party/
├── glfw/
├── glm/
├── imgui/
├── stb/
└── tinyobjloader/
```

| Library | Purpose | Management |
|---|---|---|
| Vulkan SDK | Graphics API | System installation |
| GLFW | Window creation & Vulkan surface | Git submodule (`third_party/glfw`) |
| GLM | Mathematics library | Git submodule (`third_party/glm`) |
| Dear ImGui | Runtime debugging UI | Git submodule (`third_party/imgui`) |
| stb | Image loading (`stb_image`) | Source included (`third_party/stb`) |
| tinyobjloader | Wavefront OBJ mesh loader | Header-only (`third_party/tinyobjloader`) |

The Vulkan SDK must be installed on the system. All other dependencies are bundled with the repository.

### Dependency Strategy

- **Git submodules** are used for actively maintained external libraries (GLFW, GLM, Dear ImGui).
- **Single-header or lightweight libraries** (stb, tinyobjloader) are included directly to simplify integration and reduce external dependencies.

---

## Expected output

```
[Vulkan] Instance created successfully
[Vulkan] Surface created successfully
[Vulkan] Swapchain created successfully
Image count: 3
[ObjLoader] 507 unique vertices, 2904 indices (from assets/suzanne.obj)
[ObjLoader] 165 unique vertices, 867 indices (from assets/suzanne_lod1.obj)
[ObjLoader] 34 unique vertices, 141 indices (from assets/suzanne_lod2.obj)
...
Vulkan Context initialized
[FrameRenderer] initialized (FrameGraph Stage B)
Application initialized
Application mainLoop
```

A window opens rendering the Suzanne model with GPU-driven frustum culling and dynamic LOD switching.

