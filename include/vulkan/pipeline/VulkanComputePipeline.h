#pragma once
#include <vulkan/vulkan.h>

class VulkanComputePipeline
{
public:
    void create(
        VkDevice device,
        VkDescriptorSetLayout descriptorLayout
    );
    void destroy(VkDevice device);

    VkPipeline       get()    const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};

