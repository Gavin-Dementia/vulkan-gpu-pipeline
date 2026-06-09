#include "vulkan/renderpass/VulkanFramebuffer.h"
#include <stdexcept>

void VulkanFramebuffer::create(
    VkDevice device,
    VkRenderPass renderPass,
    const std::vector<VkImageView>& imageViews,
    VkExtent2D extent)
{
    framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++)
    {
        VkImageView attachments[] = { imageViews[i] };

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass;
        info.attachmentCount = 1;
        info.pAttachments = attachments;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;

        if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create framebuffer");
    }
}

void VulkanFramebuffer::destroy(VkDevice device)
{
    for (auto fb : framebuffers)
    {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();
}

