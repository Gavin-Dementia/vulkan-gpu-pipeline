#pragma once
#include <vulkan/vulkan.h>

class VulkanPipeline
{
public:
    // Two descriptor sets since IBL Milestone 2 (see
    // docs/TECHNICAL_NOTES.md §34): set 0 = materialLayout (the grid/
    // projectile's per-object material data - VulkanDescriptor), set 1 =
    // iblLayout (ambient-lighting data shared by every material -
    // currently just the irradiance cubemap sampler, CubeSamplerDescriptor).
    // transparent selects alpha blending (srcAlpha/1-srcAlpha) and
    // disables depth writes instead of the default opaque state (no
    // blending, depth writes on) - see VulkanContext::transparentPipeline_
    // and docs/TECHNICAL_NOTES.md §43. Depth *testing* stays on either
    // way, so transparent draws are still correctly occluded by opaque
    // geometry; only depth writes are disabled, so overlapping
    // transparent instances don't occlude each other via the depth
    // buffer - correctness there relies entirely on draw order instead
    // (see sortInstances.comp).
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass,
        VkDescriptorSetLayout materialLayout,
        VkDescriptorSetLayout iblLayout,
        bool transparent = false
    );

    void destroy(VkDevice device);

    VkPipeline get() const { return pipeline; }
    VkPipelineLayout getLayout() const { return layout; }

private:
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

