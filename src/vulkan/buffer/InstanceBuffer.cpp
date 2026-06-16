#include "vulkan/buffer/InstanceBuffer.h"
#include <stdexcept>

void InstanceBuffer::create(
    VkPhysicalDevice physical, VkDevice device,
    VkCommandPool pool, VkQueue queue,
    const std::vector<InstanceData>& instances)
{
    VkDeviceSize size = sizeof(InstanceData) * instances.size();

    VulkanBuffer staging;
    staging.create(
        physical, device, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    staging.upload(device, instances.data(), size);

    buffer_.create(
        physical, device, size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,  // instance也是vertex input的一种
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    copyBuffer(device, pool, queue, staging.get(), buffer_.get(), size);
    staging.destroy(device);
}

void InstanceBuffer::destroy(VkDevice device)
{
    buffer_.destroy(device);
}

void InstanceBuffer::bind(VkCommandBuffer cmd)
{
    VkBuffer buffers[] = { buffer_.get() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 1, 1, buffers, offsets);   // binding=1
}

void InstanceBuffer::copyBuffer(
    VkDevice device, VkCommandPool pool, VkQueue queue,
    VkBuffer src, VkBuffer dst, VkDeviceSize size)
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

