cmake_minimum_required(VERSION 3.20)
project(vulkan_gpu_pipeline)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# output (portable)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# =========================
# Dependencies
# =========================

# GLFW (submodule)
add_subdirectory(third_party/glfw)

# GLM (header-only)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE third_party/glm)

# Vulkan SDK (system)
find_package(Vulkan REQUIRED)

# =========================
# Executable
# =========================
file(GLOB VULKAN_SRC
    src/core/*.cpp
    src/vulkan/command/*.cpp
    src/vulkan/core/*.cpp
    src/vulkan/device/*.cpp
    src/vulkan/frame/*.cpp
    src/vulkan/platform/*.cpp
    src/vulkan/renderpass/*.cpp
    src/vulkan/swapchain/*.cpp
    src/vulkan/*.cpp
)

add_executable(app
    src/main.cpp
    # src/core/Application.cpp
    ${VULKAN_SRC}
)

# =========================
# Include directories
# =========================

target_include_directories(app PRIVATE
    include
    src
)

# =========================
# Linking
# =========================

target_link_libraries(app PRIVATE
    glfw
    glm
    Vulkan::Vulkan
)

