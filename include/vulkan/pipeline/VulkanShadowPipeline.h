#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// 128 bytes total - exactly Vulkan's guaranteed minimum push constant
// size. `model` must match whatever triangle.vert's ubo.model is for the
// same draw (grid's shared spin rotation, or identity for the
// projectile) - the shadow map has to be cast from the same orientation
// the main pass actually renders, not the mesh's un-rotated rest pose.
struct ShadowPushConstants
{
    glm::mat4 lightViewProj;
    glm::mat4 model;
};

// Depth-only pipeline for the shadow pass: vertex stage only (no
// fragment shader, no color blend attachment), a ShadowPushConstants
// push constant - sibling to VulkanPipeline/VulkanComputePipeline, same
// "new pass stage gets its own pipeline class" precedent as the compute
// culling pipeline.
class VulkanShadowPipeline
{
public:
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass
    );

    void destroy(VkDevice device);

    VkPipeline       get()    const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};
