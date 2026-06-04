cmake_minimum_required(VERSION 3.20)
project(vulkan_gpu_pipeline)

set(CMAKE_EXE_LINKER_FLAGS "-mconsole")
set(CMAKE_CXX_STANDARD 17)

set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/../vulkan-gpu-pipeline/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUTPUT_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR})

# GLFW
add_subdirectory(third_party/glfw)

# GLM (header-only)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE third_party/glm)

# Vulkan
find_package(Vulkan REQUIRED)

# Executable
add_executable(app
    src/main.cpp
    src/core/Application.cpp
    src/vulkan/VulkanContext.cpp
)

target_include_directories(app PRIVATE
    src
)

target_link_libraries(app PRIVATE
    glfw
    glm
    Vulkan::Vulkan
)


