cmake_minimum_required(VERSION 3.20)
project(vulkan_gpu_pipeline)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================================
# Output directory
# =========================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# =========================================================
# Dependencies
# =========================================================

add_subdirectory(third_party/glfw)

add_library(glm INTERFACE)
target_include_directories(glm INTERFACE third_party/glm)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE third_party/stb)

add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE third_party/tinyobjloader)

find_package(Vulkan REQUIRED)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

# =========================================================
# ImGui core + backends
# =========================================================

set(IMGUI_DIR third_party/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

# =========================================================
# Shader System (AUTO BUILD SPIR-V)
# =========================================================

set(SHADER_SRC
    ${CMAKE_SOURCE_DIR}/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/shaders/triangle.frag
    ${CMAKE_SOURCE_DIR}/shaders/culling.comp
    ${CMAKE_SOURCE_DIR}/shaders/shadow.vert
)

set(SHADER_OUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/compiled)

file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SPIRV_SHADERS)

foreach(SHADER ${SHADER_SRC})

    get_filename_component(FILE_NAME ${SHADER} NAME)
    set(SPIRV_OUTPUT ${SHADER_OUT_DIR}/${FILE_NAME}.spv)

    add_custom_command(
        OUTPUT ${SPIRV_OUTPUT}
        COMMAND ${GLSLC_EXECUTABLE}
        ${SHADER}
        -o ${SPIRV_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader: ${FILE_NAME}"
        VERBATIM
    )

    list(APPEND SPIRV_SHADERS ${SPIRV_OUTPUT})

endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPIRV_SHADERS})

# =========================================================
# Core
# =========================================================

set(CORE_SRC
    src/core/Application.cpp
    src/core/Camera.cpp
    src/core/Projectile.cpp
)

set(UI_SRC
    src/ui/ImGuiLayer.cpp
)

# =========================================================
# Vulkan modules
# =========================================================
set(VULKAN_BUFFER_SRC
    src/vulkan/buffer/VertexBuffer.cpp
    src/vulkan/buffer/VulkanBuffer.cpp
    src/vulkan/buffer/UniformBuffer.cpp
    src/vulkan/buffer/IndexBuffer.cpp
    src/vulkan/buffer/IndirectDrawBuffer.cpp
)

set(VULKAN_COMMAND_SRC
    src/vulkan/command/VulkanCommandPool.cpp
)

set(VULKAN_CORE_SRC
    src/vulkan/VulkanContext.cpp
    src/vulkan/core/VulkanInstance.cpp
    src/vulkan/platform/VulkanSurface.cpp
    src/vulkan/device/VulkanDevice.cpp
)

set(VULKAN_DESCRIPTOR_SRC
    src/vulkan/descriptor/VulkanDescriptor.cpp
    src/vulkan/descriptor/ComputeDescriptor.cpp 
)

set(VULKAN_FRAME_SRC
    src/vulkan/frame/FrameContext.cpp
    src/vulkan/frame/FrameRenderer.cpp
    src/vulkan/frame/FrameGraph.cpp
)

set(VULKAN_PIPELINE_SRC
    src/vulkan/pipeline/VulkanPipeline.cpp
    src/vulkan/pipeline/VulkanComputePipeline.cpp
    src/vulkan/pipeline/VulkanShadowPipeline.cpp
)

set(VULKAN_RESOURCE_SRC
    src/vulkan/resource/ShaderLoader.cpp
    src/vulkan/resource/ObjLoader.cpp
)

set(VULKAN_RENDERPASS_SRC
    src/vulkan/renderpass/VulkanDepthBuffer.cpp
    src/vulkan/renderpass/VulkanRenderPass.cpp
    src/vulkan/renderpass/VulkanFramebuffer.cpp
    src/vulkan/renderpass/VulkanShadowMap.cpp
)

set(VULKAN_SWAPCHAIN_SRC
    src/vulkan/swapchain/VulkanSwapchain.cpp
)

set(VULKAN_TEXTURE_SRC
    src/vulkan/texture/VulkanTexture.cpp
)

# =========================================================
# Executable
# =========================================================

add_executable(app
    src/main.cpp
    ${CORE_SRC}
    ${VULKAN_BUFFER_SRC}
    ${VULKAN_CORE_SRC}
    ${VULKAN_COMMAND_SRC}
    ${VULKAN_DESCRIPTOR_SRC}
    ${VULKAN_FRAME_SRC}
    ${VULKAN_PIPELINE_SRC}
    ${VULKAN_RENDERPASS_SRC}
    ${VULKAN_RESOURCE_SRC}
    ${VULKAN_SWAPCHAIN_SRC}
    ${VULKAN_TEXTURE_SRC}
    ${UI_SRC} 
)

add_dependencies(app Shaders)

add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets
    COMMENT "Copying assets to output directory"
)

# =========================================================
# Include directories
# =========================================================

target_include_directories(app PRIVATE
    include
    src
)

# =========================================================
# Linking
# =========================================================

target_link_libraries(app PRIVATE
    glfw
    glm
    tinyobjloader
    imgui   
    stb
    Vulkan::Vulkan
)

