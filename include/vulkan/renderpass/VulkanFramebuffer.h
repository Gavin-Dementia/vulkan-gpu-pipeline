#pragma once

#include <vulkan/vulkan.h>
#include <vector>

class VulkanFramebuffer
{
public:
    void create(VkDevice device,
                VkRenderPass renderPass,
                const std::vector<VkImageView>& imageViews,
                VkExtent2D extent);

    void destroy(VkDevice device);

    const std::vector<VkFramebuffer>& get() const { return framebuffers; }

private:
    std::vector<VkFramebuffer> framebuffers;
};

