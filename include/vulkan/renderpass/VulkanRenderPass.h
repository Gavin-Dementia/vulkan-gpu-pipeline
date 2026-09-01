#pragma once

#include <vulkan/vulkan.h>

class VulkanRenderPass
{
public:
    void create(
        VkDevice device,
        VkFormat colorFormat,
        VkFormat depthFormat);

    // Depth-only variant for the shadow map pass - no color attachment,
    // final layout leaves the depth image ready to be sampled by the main
    // geometry pass's fragment shader.
    void createDepthOnly(
        VkDevice device,
        VkFormat depthFormat);

    void destroy(VkDevice device);

    VkRenderPass get() const { return renderPass; }

private:
    VkRenderPass renderPass = VK_NULL_HANDLE;
};

