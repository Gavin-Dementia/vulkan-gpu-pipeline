# Vulkan GPU Pipeline

A minimal Vulkan rendering framework for learning GPU pipeline architecture.

---

# 🔧 Requirements (per machine)

You only need to install:

## 1. Vulkan SDK
Download from:

https://vulkan.lunarg.com/sdk/home

version: 1.4.350.0 12-May-2026
- 3-4 G require
install components
- Installs the GLM headers.
- Install debug shader toolchain libraries.
- Installs the VMA header file.

Make sure:
- `vulkaninfo` works in terminal
- Environment variables are set correctly

---

## 2. C++ Compiler

Choose one:

### Windows (recommended cause it's WIP)
- Visual Studio 2022
  - Desktop development with C++
  - Windows 10/11 SDK

OR

### MinGW (experimental)
- g++ (MinGW-w64)

---

## 3. CMake
Version >= 3.20

Verify:
```bash
cmake --version
```

# 📦 Clone Repository

IMPORTANT: this repo uses git submodules
```bash
git clone --recursive https://github.com/your_repo/vulkan-gpu-pipeline.git
```
If already cloned:
```bash
git submodule update --init --recursive
```

# 🧱 Build Instructions

```bash
cmake -S . -B build
cmake --build build
```
Executable will be in:
```bash
build/bin/
```

# 📁 Dependencies (managed in repo)
This project does NOT require external installs for:

- GLFW (submodule)
- GLM (submodule)

Only Vulkan SDK is required externally.

# 🚀 First Run
Expected output:
```bash
GLFW window opens
Vulkan instance created successfully
Physical devices listed
```
# ⚠️ Notes
- Do NOT manually install GLFW or GLM
- Only Vulkan SDK is system dependency
- Keep submodules updated

