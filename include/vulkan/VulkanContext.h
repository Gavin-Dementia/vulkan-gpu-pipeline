#pragma once

#include <GLFW/glfw3.h>
#include <vector>
#include "core/Camera.h"
#include "ui/ImGuiLayer.h"

#include "vulkan/buffer/VertexBuffer.h"
#include "vulkan/buffer/UniformBuffer.h"
#include "vulkan/command/VulkanCommandPool.h"
#include "vulkan/core/VulkanInstance.h"
#include "vulkan/culling/Frustum.h"
#include "vulkan/device/VulkanDevice.h"
#include "vulkan/descriptor/ComputeDescriptor.h"
#include "vulkan/descriptor/VulkanDescriptor.h"
#include "vulkan/platform/VulkanSurface.h"
#include "vulkan/renderpass/VulkanDepthBuffer.h"
#include "vulkan/renderpass/VulkanFramebuffer.h"
#include "vulkan/renderpass/VulkanRenderPass.h"
#include "vulkan/swapchain/VulkanSwapchain.h"
#include "vulkan/pipeline/VulkanPipeline.h"
#include "vulkan/pipeline/VulkanComputePipeline.h"
#include "vulkan/buffer/IndexBuffer.h"
#include "vulkan/instance/InstanceData.h"
#include "vulkan/buffer/IndirectDrawBuffer.h"
#include "vulkan/texture/VulkanTexture.h"


class VulkanContext
{
public:
    static constexpr uint32_t GRID_SIZE = 7;
    static constexpr uint32_t OBJECT_COUNT = 
                                GRID_SIZE * GRID_SIZE * GRID_SIZE;  // 343
    struct LODMesh
    {
        VertexBuffer   vertexBuffer;
        IndexBuffer    indexBuffer;
        VulkanBuffer   visibleInstanceBuffer;
        IndirectDrawBuffer indirectDrawBuffer;
    };

    std::array<uint32_t, 3> lastVisibleCount_ = {0, 0, 0};
    void setLastVisibleCount(int lod, uint32_t c)   { lastVisibleCount_[lod] = c; }
    uint32_t getLastVisibleCount(int lod) const     { return lastVisibleCount_[lod]; }

    void init(GLFWwindow* window);
    void cleanup();

    GLFWwindow* window()     { return window_; }
    Camera&     camera()     { return camera_; }
    ImGuiLayer& imguiLayer() { return imguiLayer_; }

    VulkanDevice&          device()            { return device_; }
    VulkanSwapchain&       swapchain()         { return swapchain_; }
    VulkanInstance&        instance()          { return instance_; }
    VulkanSurface&         surface()           { return surface_; }
    VulkanCommandPool&     commandPool()       { return commandPool_; }
    VulkanRenderPass&      renderPass()        { return renderPass_; }
    VulkanFramebuffer&     framebuffer()       { return framebuffer_; }
    VulkanPipeline&        pipeline()          { return pipeline_; }
    UniformBuffer&         uniformBuffer()     { return uniformBuffer_; }
    VulkanDescriptor&      descriptor()        { return descriptor_; }
    VulkanDepthBuffer&     depthBuffer()       { return depthBuffer_; }
    VulkanComputePipeline& computePipeline()   { return computePipeline_; }
    ComputeDescriptor&     computeDescriptor() { return computeDescriptor_; }
    VulkanBuffer&          frustumBuffer()     { return frustumBuffer_; }
    VulkanBuffer&          objectBuffer()      { return objectBuffer_; }
    VulkanTexture&         texture()           { return texture_; }

    LODMesh& lod(int level) { return lods_[level]; }


private:

    void initCore();
    void initSceneData();
    void initCullingResources();

    std::vector<InstanceData> cachedInstances_;
    std::array<LODMesh, 3> lods_;

    GLFWwindow* window_ = nullptr;
    Camera camera_;
    ImGuiLayer imguiLayer_;

    VulkanInstance        instance_;
    VulkanSurface         surface_;
    VulkanDevice          device_;
    VulkanSwapchain       swapchain_;
    VulkanCommandPool     commandPool_;
    VulkanRenderPass      renderPass_;
    VulkanFramebuffer     framebuffer_;
    VulkanPipeline        pipeline_;
    UniformBuffer         uniformBuffer_;
    VulkanDescriptor      descriptor_;
    VulkanDepthBuffer     depthBuffer_;
    VulkanBuffer          objectBuffer_;
    ComputeDescriptor     computeDescriptor_;
    VulkanComputePipeline computePipeline_;
    VulkanBuffer          frustumBuffer_;
    VulkanTexture         texture_;



};

