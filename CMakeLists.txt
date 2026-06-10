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
# Core
# =========================
set(CORE_SRC
    src/core/Application.cpp
)

# =========================
# Vulkan Core
# =========================
set(VULKAN_CORE_SRC
    src/vulkan/VulkanContext.cpp
    src/vulkan/core/VulkanInstance.cpp
    src/vulkan/platform/VulkanSurface.cpp
    src/vulkan/device/VulkanDevice.cpp
)

# =========================
# Vulkan Command
# =========================
set(VULKAN_COMMAND_SRC
    src/vulkan/command/VulkanCommandPool.cpp
)

# =========================
# Vulkan Frame System
# =========================
set(VULKAN_FRAME_SRC
    src/vulkan/frame/FrameContext.cpp
    src/vulkan/frame/FrameRenderer.cpp
    src/vulkan/frame/FrameGraph.cpp
)

# =========================
# Vulkan RenderPass
# =========================
set(VULKAN_RENDERPASS_SRC
    src/vulkan/renderpass/VulkanRenderPass.cpp
    src/vulkan/renderpass/VulkanFramebuffer.cpp
)

# =========================
# Vulkan Swapchain
# =========================
set(VULKAN_SWAPCHAIN_SRC
    src/vulkan/swapchain/VulkanSwapchain.cpp
)

# =========================
# Vulkan Sync
# =========================
set(VULKAN_SYNC_SRC
)

# =========================
# Executable
# =========================
add_executable(app
    src/main.cpp
    ${CORE_SRC}
    ${VULKAN_CORE_SRC}
    ${VULKAN_COMMAND_SRC}
    ${VULKAN_FRAME_SRC}
    ${VULKAN_RENDERPASS_SRC}
    ${VULKAN_SWAPCHAIN_SRC}
    ${VULKAN_SYNC_SRC}
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

