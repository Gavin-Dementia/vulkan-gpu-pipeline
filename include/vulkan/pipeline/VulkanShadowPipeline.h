#pragma once
#include <vulkan/vulkan.h>

// Depth-only pipeline for the shadow pass: vertex stage only (no
// fragment shader, no color blend attachment), a single mat4 push
// constant (light-space view-projection) - sibling to VulkanPipeline/
// VulkanComputePipeline, same "new pass stage gets its own pipeline
// class" precedent as the compute culling pipeline.
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
