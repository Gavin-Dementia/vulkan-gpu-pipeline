#include "vulkan/buffer/IndirectDrawBuffer.h"

void IndirectDrawBuffer::create(VkPhysicalDevice physical, VkDevice device, uint32_t indexCount)
{
    buffer_.create(
        physical, device, sizeof(DrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    resetInstanceCount(device, indexCount);
}

void IndirectDrawBuffer::resetInstanceCount(VkDevice device, uint32_t indexCount)
{
    DrawIndirectCommand cmd{};
    cmd.indexCount    = indexCount;
    cmd.instanceCount = 0;   // reset each frame, accumulated by compute shader累加
    cmd.firstIndex    = 0;
    cmd.vertexOffset  = 0;
    cmd.firstInstance = 0;

    buffer_.upload(device, &cmd, sizeof(DrawIndirectCommand));
}

void IndirectDrawBuffer::destroy(VkDevice device)
{
    buffer_.destroy(device);
}

