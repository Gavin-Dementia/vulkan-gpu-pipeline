#pragma once

#include <vulkan/vulkan.h>
#include <vector>

class VulkanFramebuffer
{
public:
    void create(VkDevice device,
                VkRenderPass renderPass,
                const std::vector<VkImageView>& imageViews,
                VkImageView depthView,
                VkExtent2D extent);

    // Single fixed-size framebuffer with one depth attachment - for the
    // shadow map, which isn't swapchain-sized and has no color attachment.
    void createDepthOnly(VkDevice device,
                          VkRenderPass renderPass,
                          VkImageView depthView,
                          VkExtent2D extent);

    void destroy(VkDevice device);

    const std::vector<VkFramebuffer>& get() const { return framebuffers; }

private:
    std::vector<VkFramebuffer> framebuffers;
};

