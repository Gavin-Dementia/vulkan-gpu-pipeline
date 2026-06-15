#pragma once
#include <vulkan/vulkan.h>

class VulkanDepthBuffer
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkExtent2D extent
    );
    void destroy(VkDevice device);

    VkImageView view() const { return view_; }
    VkFormat    format() const { return format_; }

private:
    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
    VkFormat       format_ = VK_FORMAT_D32_SFLOAT;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};

