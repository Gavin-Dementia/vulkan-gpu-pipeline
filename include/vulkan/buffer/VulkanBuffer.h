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

    void download(VkDevice device, void* outData, VkDeviceSize size);
    
    VkBuffer get() const { return buffer_; }

private:
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;

    // Persistently mapped for the buffer's lifetime when created with
    // HOST_VISIBLE memory, so upload()/download() are plain memcpy calls
    // instead of paying a vkMapMemory/vkUnmapMemory pair every call
    // (several buffers - UBO, frustum, indirect draw - are touched once
    // per frame). Left nullptr for DEVICE_LOCAL-only buffers.
    void* mapped_ = nullptr;

    // find the right constitutional memory type
    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};

