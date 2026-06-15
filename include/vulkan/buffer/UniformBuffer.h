#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "vulkan/buffer/VulkanBuffer.h"

struct UBOData
{
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

class UniformBuffer
{
public:
    void create(VkPhysicalDevice physical, VkDevice device);
    void update(VkDevice device, const UBOData& data);
    void destroy(VkDevice device);

    VkBuffer get() const { return buffer_.get(); }

private:
    VulkanBuffer buffer_;
};

