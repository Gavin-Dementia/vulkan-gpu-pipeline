cmake_minimum_required(VERSION 3.20)
project(vulkan_gpu_pipeline)

set(CMAKE_CXX_STANDARD 17)

# output (portable)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GLFW (submodule)
add_subdirectory(third_party/glfw)

# GLM (header-only)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE third_party/glm)

# Vulkan SDK (system install only)
find_package(Vulkan REQUIRED)

add_executable(app
    src/main.cpp
    src/core/Application.cpp
    src/vulkan/VulkanContext.cpp
)

target_include_directories(app PRIVATE src)

target_link_libraries(app PRIVATE
    glfw
    glm
    Vulkan::Vulkan
)

