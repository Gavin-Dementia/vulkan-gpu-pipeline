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
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass,
        VkDescriptorSetLayout materialLayout,
        VkDescriptorSetLayout iblLayout
    );

    void destroy(VkDevice device);

    VkPipeline get() const { return pipeline; }
    VkPipelineLayout getLayout() const { return layout; }

private:
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

