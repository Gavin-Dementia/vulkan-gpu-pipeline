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

    // Offscreen scene variant for the dockable ImGui "Viewport" panel -
    // color + depth like create(), but the color attachment's final layout
    // leaves it ready to be sampled by ImGui's fragment shader instead of
    // presented, and it isn't tied to the swapchain's format/extent.
    void createOffscreenColor(
        VkDevice device,
        VkFormat colorFormat,
        VkFormat depthFormat);

    void destroy(VkDevice device);

    VkRenderPass get() const { return renderPass; }

private:
    VkRenderPass renderPass = VK_NULL_HANDLE;
};

