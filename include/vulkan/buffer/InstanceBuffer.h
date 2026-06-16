#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "vulkan/buffer/VulkanBuffer.h"
#include "vulkan/instance/InstanceData.h"

class InstanceBuffer
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        const std::vector<InstanceData>& instances
    );

    void destroy(VkDevice device);
    void bind(VkCommandBuffer cmd);   
    // binding=1 ,seperate with vertex buffer's binding=0

private:
    VulkanBuffer buffer_;

    void copyBuffer(
        VkDevice device, VkCommandPool pool, VkQueue queue,
        VkBuffer src, VkBuffer dst, VkDeviceSize size
    );
};

