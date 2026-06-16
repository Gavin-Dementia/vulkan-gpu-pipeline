#include "vulkan/buffer/IndexBuffer.h"
#include <stdexcept>

void IndexBuffer::create(
    VkPhysicalDevice physical,
    VkDevice device,
    VkCommandPool pool,
    VkQueue queue,
    const std::vector<uint32_t>& indices)
{
    count_ = static_cast<uint32_t>(indices.size());
    VkDeviceSize size = sizeof(uint32_t) * indices.size();

    VulkanBuffer staging;
    staging.create(
        physical, device, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    staging.upload(device, indices.data(), size);

    buffer_.create(
        physical, device, size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  // ← 关键差异
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    copyBuffer(device, pool, queue, staging.get(), buffer_.get(), size);
    staging.destroy(device);
}

void IndexBuffer::destroy(VkDevice device)
{
    buffer_.destroy(device);
}

void IndexBuffer::bind(VkCommandBuffer cmd)
{
    vkCmdBindIndexBuffer(cmd, buffer_.get(), 0, VK_INDEX_TYPE_UINT32);
}

void IndexBuffer::copyBuffer(
    VkDevice device,
    VkCommandPool pool,
    VkQueue queue,
    VkBuffer src,
    VkBuffer dst,
    VkDeviceSize size)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copyRegion);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

