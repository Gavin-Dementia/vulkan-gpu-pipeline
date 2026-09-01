#include "vulkan/renderpass/VulkanFramebuffer.h"
#include <stdexcept>
#include <array>

void VulkanFramebuffer::create(
    VkDevice device,
    VkRenderPass renderPass,
    const std::vector<VkImageView>& imageViews,
    VkImageView depthView,  
    VkExtent2D extent)
{
    framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++)
    {  
        std::array<VkImageView, 2> attachments = {
            imageViews[i],
            depthView
        };

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;

        if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create framebuffer");
    }
}

void VulkanFramebuffer::createDepthOnly(
    VkDevice device,
    VkRenderPass renderPass,
    VkImageView depthView,
    VkExtent2D extent)
{
    framebuffers.resize(1);

    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass;
    info.attachmentCount = 1;
    info.pAttachments = &depthView;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;

    if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[0]) != VK_SUCCESS)
        throw std::runtime_error("Failed to create depth-only framebuffer");
}

void VulkanFramebuffer::destroy(VkDevice device)
{
    for (auto fb : framebuffers)
    {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();
}

