#include "vulkan/texture/VulkanCubemap.h"
#include <stdexcept>

void VulkanCubemap::create(
    VkPhysicalDevice physical,
    VkDevice device,
    uint32_t faceSize,
    VkFormat format,
    VkImageUsageFlags usage)
{
    faceSize_ = faceSize;

    // No staging buffer/upload here, unlike VulkanTexture - the image
    // starts UNDEFINED and is filled entirely by whatever render pass
    // bakes content into its 6 face views (see initEnvironment() in
    // VulkanContext.cpp).
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format;
    imageInfo.extent        = { faceSize, faceSize, 1 };
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 6;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &imageInfo, nullptr, &image_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create cubemap image");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, image_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physical, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory_) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate cubemap memory");

    vkBindImageMemory(device, image_, memory_, 0);

    // Cube view - all 6 layers, for sampling as samplerCube.
    VkImageViewCreateInfo cubeViewInfo{};
    cubeViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeViewInfo.image                           = image_;
    cubeViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeViewInfo.format                          = format;
    cubeViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    cubeViewInfo.subresourceRange.baseMipLevel   = 0;
    cubeViewInfo.subresourceRange.levelCount     = 1;
    cubeViewInfo.subresourceRange.baseArrayLayer = 0;
    cubeViewInfo.subresourceRange.layerCount     = 6;

    if (vkCreateImageView(device, &cubeViewInfo, nullptr, &cubeView_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create cubemap cube view");

    // 6 face views - one array layer each, for render-pass attachments.
    for (uint32_t i = 0; i < 6; i++)
    {
        VkImageViewCreateInfo faceViewInfo{};
        faceViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        faceViewInfo.image                           = image_;
        faceViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        faceViewInfo.format                          = format;
        faceViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        faceViewInfo.subresourceRange.baseMipLevel   = 0;
        faceViewInfo.subresourceRange.levelCount     = 1;
        faceViewInfo.subresourceRange.baseArrayLayer = i;
        faceViewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &faceViewInfo, nullptr, &faceViews_[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create cubemap face view");
    }

    // CLAMP_TO_EDGE on all 3 axes - REPEAT (VulkanTexture's default) is
    // meaningless for a cube; edge clamping avoids seam artifacts at face
    // boundaries under linear filtering.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter     = VK_FILTER_LINEAR;
    samplerInfo.minFilter     = VK_FILTER_LINEAR;
    samplerInfo.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor   = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp     = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create cubemap sampler");
}

void VulkanCubemap::destroy(VkDevice device)
{
    vkDestroySampler(device, sampler_, nullptr);
    for (auto view : faceViews_)
        vkDestroyImageView(device, view, nullptr);
    vkDestroyImageView(device, cubeView_, nullptr);
    vkDestroyImage(device, image_, nullptr);
    vkFreeMemory(device, memory_, nullptr);
}

uint32_t VulkanCubemap::findMemoryType(
    VkPhysicalDevice physical, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    throw std::runtime_error("Failed to find suitable memory type");
}
