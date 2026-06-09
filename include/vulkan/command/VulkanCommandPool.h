#pragma once

#include <vulkan/vulkan.h>
#include <vector>

class VulkanCommandPool
{
public:
    void create(VkDevice device, uint32_t queueFamilyIndex);
    void destroy(VkDevice device);

    VkCommandBuffer allocateCommandBuffer(VkDevice device);

    VkCommandPool get() const { return commandPool; }

private:
    VkCommandPool commandPool = VK_NULL_HANDLE;
};

