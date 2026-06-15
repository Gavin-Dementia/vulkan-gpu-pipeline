#pragma once
#include <vulkan/vulkan.h>

class VulkanBuffer
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties
    );

    void destroy(VkDevice device);

    // CPU write data into this buffer
    // only effected with HOST_VISIBLE's buffer
    void upload(VkDevice device, const void* data, VkDeviceSize size);

    VkBuffer get() const { return buffer_; }

private:
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;

    // find the right constitutional memory type
    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};

