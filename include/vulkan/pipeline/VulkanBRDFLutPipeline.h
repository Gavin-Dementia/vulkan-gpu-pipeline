#pragma once
#include <vulkan/vulkan.h>

// One-shot pipeline for baking the BRDF integration LUT (see
// VulkanContext::initEnvironment(), docs/TECHNICAL_NOTES.md §35) -
// fullscreenTriangle.vert + brdfLUT.frag. Zero descriptor sets, zero
// push constants - brdfLUT.frag is a pure function of the fullscreen
// triangle's own UV, unlike every other bake pipeline in this codebase
// (even VulkanEnvCapturePipeline, which has no descriptor set but still
// needs a push constant for direction reconstruction). Depth disabled.
class VulkanBRDFLutPipeline
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
