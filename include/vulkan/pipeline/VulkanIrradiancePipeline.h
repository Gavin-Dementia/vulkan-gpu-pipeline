#pragma once
#include <vulkan/vulkan.h>
#include "vulkan/pipeline/VulkanSkyboxPipeline.h"   // reuses SkyboxPushConstants - see docs/TECHNICAL_NOTES.md §34

// One-shot pipeline for baking cosine-weighted diffuse irradiance into
// irradianceCubemap_'s 6 faces (see VulkanContext::initEnvironment()) -
// fullscreenTriangle.vert + irradianceConvolve.frag, 1 descriptor set
// (CubeSamplerDescriptor's layout - bound to environmentCubemap_ at bake
// time via skyboxDescriptor_, the same instance the live skybox draw
// uses), same push-constant range as VulkanSkyboxPipeline (identical
// data need: reconstruct a world direction from invViewProj + cameraPos,
// nothing extra). Depth disabled (no depth attachment in its render
// pass). Sibling to VulkanEnvCapturePipeline/VulkanSkyboxPipeline.
class VulkanIrradiancePipeline
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
