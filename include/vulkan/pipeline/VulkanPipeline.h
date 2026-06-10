#pragma once
#include <vulkan/vulkan.h>

class VulkanPipeline
{
public:
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass);

    void destroy(VkDevice device);

    VkPipeline get() const { return pipeline; }
    VkPipelineLayout getLayout() const { return layout; }

private:
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

