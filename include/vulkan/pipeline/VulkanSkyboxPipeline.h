#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// Matches skybox.frag's push constant block exactly.
struct SkyboxPushConstants
{
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
};

// Live skybox pipeline: fullscreenTriangle.vert + skybox.frag, one
// descriptor set (CubeSamplerDescriptor - the environment cubemap sampler),
// depth test/write both disabled (drawn first inside GeometryPass,
// before the grid - see FrameRenderer.cpp; the grid's own depth test
// afterward naturally overdraws it, no z-value trickery needed). Sibling
// to VulkanEnvCapturePipeline/VulkanShadowPipeline.
class VulkanSkyboxPipeline
{
public:
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass,
        VkDescriptorSetLayout descriptorLayout
    );

    void destroy(VkDevice device);

    VkPipeline       get()    const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};
