#pragma once
#include <vulkan/vulkan.h>

class VulkanCommandPool {
public:
    void create(VkDevice device, uint32_t queueFamilyIndex);
    void destroy(VkDevice device);

    VkCommandPool get() const { return commandPool; }

private:
    VkCommandPool commandPool = VK_NULL_HANDLE;
};

