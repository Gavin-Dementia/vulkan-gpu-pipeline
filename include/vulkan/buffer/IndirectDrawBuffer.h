#pragma once
#include <vulkan/vulkan.h>
#include "vulkan/buffer/VulkanBuffer.h"

struct DrawIndirectCommand
{
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t firstInstance;
};

class IndirectDrawBuffer
{
public:
    void create(VkPhysicalDevice physical, VkDevice device, uint32_t indexCount);
    void resetInstanceCount(VkDevice device, uint32_t indexCount);
    void destroy(VkDevice device);

    uint32_t getVisibleCount(VkDevice device);
    VkBuffer get() const { return buffer_.get(); }

private:
    VulkanBuffer buffer_;
};

