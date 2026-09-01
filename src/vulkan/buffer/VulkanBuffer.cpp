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

    // looking for memory type of buffer needed
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

    // Map once up front for HOST_VISIBLE buffers instead of mapping on
    // every upload()/download() call - safe because HOST_COHERENT is
    // always requested alongside HOST_VISIBLE in this codebase, so no
    // explicit flush/invalidate is needed between CPU writes and GPU reads.
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(device, memory_, 0, size, 0, &mapped_);
}

void VulkanBuffer::destroy(VkDevice device)
{
    if (mapped_)
    {
        vkUnmapMemory(device, memory_);
        mapped_ = nullptr;
    }

    vkDestroyBuffer(device, buffer_, nullptr);
    vkFreeMemory(device, memory_, nullptr);
}

void VulkanBuffer::upload(VkDevice device, const void* data, VkDeviceSize size)
{
    memcpy(mapped_, data, size);
}

void VulkanBuffer::download(VkDevice device, void* outData, VkDeviceSize size)
{
    memcpy(outData, mapped_, size);
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

