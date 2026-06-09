#include "vulkan/command/VulkanCommandPool.h"
#include <stdexcept>

void VulkanCommandPool::create(VkDevice device, uint32_t queueFamilyIndex)
{
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.queueFamilyIndex = queueFamilyIndex;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(device, &info, nullptr, &commandPool) != VK_SUCCESS)
        throw std::runtime_error("Failed to create CommandPool");
}

void VulkanCommandPool::destroy(VkDevice device)
{
    if (commandPool)
    {
        vkDestroyCommandPool(device, commandPool, nullptr);
        commandPool = VK_NULL_HANDLE;
    }
}

VkCommandBuffer VulkanCommandPool::allocateCommandBuffer(VkDevice device)
{
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = commandPool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &info, &cmd);

    return cmd;
}

