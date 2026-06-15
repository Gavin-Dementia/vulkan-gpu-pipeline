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

    // 把CPU数据写进这个buffer（只对HOST_VISIBLE的buffer有效）
    void upload(VkDevice device, const void* data, VkDeviceSize size);

    VkBuffer get() const { return buffer_; }

private:
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;

    // 找到符合条件的memory type
    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};

