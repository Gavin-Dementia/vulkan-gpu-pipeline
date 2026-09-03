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

    // Color-only variant, no depth attachment at all - for passes that are
    // a pure per-pixel function with no geometry/depth complexity, e.g.
    // baking a cubemap face (see VulkanCubemap / "Hierarchical / IBL"
    // module notes in architecture.md). finalLayout leaves the color
    // image ready to be sampled afterward, same reasoning as
    // createOffscreenColor()'s color attachment.
    void createColorOnly(
        VkDevice device,
        VkFormat colorFormat);

    void destroy(VkDevice device);

    VkRenderPass get() const { return renderPass; }

private:
    VkRenderPass renderPass = VK_NULL_HANDLE;
};

