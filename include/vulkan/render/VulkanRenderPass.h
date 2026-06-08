#pragma once
#include <vulkan/vulkan.h>

class VulkanRenderPass {
public:
    void create(VkDevice device, VkFormat swapchainFormat);
    void destroy(VkDevice device);

    VkRenderPass get() const { return renderPass; }

private:
    VkRenderPass renderPass = VK_NULL_HANDLE;
};

