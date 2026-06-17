# Vulkan GPU Pipeline

A GPU-driven rendering framework built from scratch in C++17 and Vulkan,
designed around a **FrameGraph DAG** for explicit pass dependency and execution ordering.

---

## Current state

| Feature | Status |
|---|---|
| Vulkan instance / device / swapchain | ✅ |
| RenderPass + Framebuffer | ✅ |
| FrameGraph (DAG, topological sort) | ✅ |
| Vertex Buffer (staging → DEVICE_LOCAL) | ✅ |
| OBJ mesh loading (tinyobjloader) | ✅ |
| Double buffering + sync primitives | ✅ |
| Uniform Buffer (MVP matrix) | ✅ |
| Depth Buffer | ✅ |
| Texture Sampling | 🔲 |
| Compute Pass (GPU Culling) | ✅ |

---

## Architecture

```
Application
└── VulkanContext          device, swapchain, renderpass, pipeline
    └── FrameRenderer      per-frame sync, command recording
        └── FrameGraph     DAG of render passes
            ├── GeometryPass   → bind VB, draw
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

| Library | How managed |
|---|---|
| GLFW | git submodule (`third_party/glfw`) |
| GLM | git submodule (`third_party/glm`) |
| tinyobjloader | header-only (`third_party/tinyobjloader`) |
| Vulkan | system install (Vulkan SDK) |

Do **not** install GLFW or GLM separately — they are bundled.

---

## Expected output

```
[Vulkan] Instance created successfully
[Vulkan] Surface created successfully
[Vulkan] Swapchain created successfully
Image count: 3
[ObjLoader] loaded 2904 vertices from assets/suzanne.obj
Vulkan Context initialized
[FrameRenderer] initialized (FrameGraph DAG)
Application initialized
Application mainLoop
```

A window opens rendering the Suzanne mesh.

