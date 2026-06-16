#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "vulkan/buffer/VulkanBuffer.h"

class IndexBuffer
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        const std::vector<uint32_t>& indices
    );

    void destroy(VkDevice device);
    void bind(VkCommandBuffer cmd);

    uint32_t indexCount() const { return count_; }

private:
    VulkanBuffer buffer_;
    uint32_t count_ = 0;

    void copyBuffer(
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        VkBuffer src,
        VkBuffer dst,
        VkDeviceSize size
    );
};

