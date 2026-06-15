#include "vulkan/buffer/VulkanBuffer.h"
#include <stdexcept>
#include <cstring>

void VulkanBuffer::create(
    VkPhysicalDevice physical,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create buffer");

    // 查询这个buffer需要什么样的内存
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buffer_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        physical,
        memReq.memoryTypeBits,
        properties
    );

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory_) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate buffer memory");

    vkBindBufferMemory(device, buffer_, memory_, 0);
}

void VulkanBuffer::destroy(VkDevice device)
{
    vkDestroyBuffer(device, buffer_, nullptr);
    vkFreeMemory(device, memory_, nullptr);
}

void VulkanBuffer::upload(VkDevice device, const void* data, VkDeviceSize size)
{
    void* mapped;
    vkMapMemory(device, memory_, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device, memory_);
}

uint32_t VulkanBuffer::findMemoryType(
    VkPhysicalDevice physical,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        bool typeMatch = typeFilter & (1 << i);
        bool propMatch = (memProps.memoryTypes[i].propertyFlags & properties) == properties;

        if (typeMatch && propMatch)
            return i;
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

