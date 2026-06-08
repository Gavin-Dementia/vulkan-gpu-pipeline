#include "vulkan/render/VulkanFramebuffer.h"
#include <stdexcept>
#include <iostream>

void VulkanFramebuffer::create(
    VkDevice device,
    VkRenderPass renderPass_,
    const std::vector<VkImageView>& imageViews,
    VkExtent2D extent_)
{
    renderPass = renderPass_;
    extent = extent_;

    framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++) {

        VkImageView attachments[] = {
            imageViews[i]
        };

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass;
        info.attachmentCount = 1;
        info.pAttachments = attachments;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;

        if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }

        std::cout << "[Vulkan] Framebuffer created: " << i << "\n";
    }
}

void VulkanFramebuffer::destroy(VkDevice device) {
    for (auto fb : framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();
}

