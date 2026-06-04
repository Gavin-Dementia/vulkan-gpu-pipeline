# Setup Guide (Vulkan GPU Pipeline)

This document describes detailed setup and build process for developers.

---

# 1. System Requirements

## Required

- Windows 10/11
- CMake >= 3.20
- C++ Compiler:
  - MSVC (recommended)
  - or MinGW (experimental)

---

# 2. Vulkan SDK

Install:
https://vulkan.lunarg.com/sdk/home

Version:
- 1.4.350.0 (recommended)

Ensure:
- vulkaninfo works
- PATH configured correctly

---

# 3. Repository Dependencies

This project uses git submodules:

```bash id="r4"
git clone --recursive <repo>
```

If already cloned:

```bash id="r4"
git submodule update --init --recursive
```

Includes:

- GLFW (window + surface)
- GLM (math library)

# 4. Build System
```bash id="r4"
cmake -S . -B build
cmake --build build
```

Output:
```bash id="r4"
build/bin/app.exe
```

# 5. CMake Design Notes

- GLFW is built as submodule
- GLM is header-only
- Vulkan is system dependency only

No external package manager required.

# 6. Common Issues

Vulkan not found
- Check Vulkan SDK install
- Ensure environment variables set

GLFW build fails
- Ensure submodule initialized

WinMain error
- Ensure CMake uses console subsystem

# 7. Architecture Note

This project is intentionally minimal:
```bash id="r4"
GLFW → window
VulkanContext → instance/device
Renderer → future expansion
```

