#include "vulkan/buffer/VertexBuffer.h"
#include <stdexcept>

void VertexBuffer::create(
    VkPhysicalDevice physical,
    VkDevice device,
    VkCommandPool pool,
    VkQueue queue,
    const std::vector<Vertex>& vertices)
{
    count_ = static_cast<uint32_t>(vertices.size());
    VkDeviceSize size = sizeof(Vertex) * vertices.size();

    // 1. 建staging buffer（CPU可写）
    VulkanBuffer staging;
    staging.create(
        physical, device, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // 2. 把顶点数据写进staging
    staging.upload(device, vertices.data(), size);

    // 3. 建真正的vertex buffer（GPU专用）
    buffer_.create(
        physical, device, size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    // 4. GPU把staging的数据copy过来
    copyBuffer(device, pool, queue, staging.get(), buffer_.get(), size);

    // 5. staging buffer完成使命，销毁
    staging.destroy(device);
}

void VertexBuffer::destroy(VkDevice device)
{
    buffer_.destroy(device);
}

void VertexBuffer::bind(VkCommandBuffer cmd)
{
    VkBuffer buffers[] = { buffer_.get() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
}

void VertexBuffer::copyBuffer(
    VkDevice device,
    VkCommandPool pool,
    VkQueue queue,
    VkBuffer src,
    VkBuffer dst,
    VkDeviceSize size)
{
    // 一次性command buffer
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

    // submit，等GPU完成
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);  // 等这次transfer完成

    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

