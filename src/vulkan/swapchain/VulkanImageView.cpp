#include "vulkan/swapchain/VulkanImageView.h"
#include <iostream>
#include <stdexcept>

VkImageView VulkanImageView::createImageView(
    VkDevice device,
    VkImage image,
    VkFormat format) {

    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;

    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;

    // color attachment only
    info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;

    VkImageView imageView;

    if (vkCreateImageView(device, &info, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create ImageView");
    }
    std::cout<<"Success to create ImageView"<< std::endl;

    return imageView;
}

void VulkanImageView::create(
    VkDevice device,
    const std::vector<VkImage>& images,
    VkFormat format) {

    imageViews.resize(images.size());

    for (size_t i = 0; i < images.size(); i++) {
        imageViews[i] = createImageView(device, images[i], format);
    }

    std::cout << "[Vulkan] ImageViews created: " << imageViews.size() << "\n";
}

void VulkanImageView::destroy(VkDevice device) {

    for (auto view : imageViews) {
        vkDestroyImageView(device, view, nullptr);
    }

    imageViews.clear();
}

