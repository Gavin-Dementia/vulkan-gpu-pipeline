#include "vulkan/command/VulkanCommandPool.h"
#include <stdexcept>
#include <iostream>

void VulkanCommandPool::create(VkDevice device, uint32_t queueFamilyIndex) {

    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queueFamilyIndex;

    if (vkCreateCommandPool(device, &info, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }

    std::cout << "[Vulkan] CommandPool created\n";
}

void VulkanCommandPool::destroy(VkDevice device) {
    if (commandPool) {
        vkDestroyCommandPool(device, commandPool, nullptr);
    }
}

