#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <array>

#include "vulkan/buffer/VulkanBuffer.h"

struct Vertex
{
    glm::vec3 position;  // vec2 → vec3

    static VkVertexInputBindingDescription getBindingDescription()
    {
        VkVertexInputBindingDescription desc{};
        desc.binding   = 0;
        desc.stride    = sizeof(Vertex);
        desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return desc;
    }

    static std::array<VkVertexInputAttributeDescription, 1> getAttributeDescriptions()
    {
        std::array<VkVertexInputAttributeDescription, 1> attrs{};
        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;  // vec2 → vec3
        attrs[0].offset   = offsetof(Vertex, position);
        return attrs;
    }
};

class VertexBuffer
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        const std::vector<Vertex>& vertices
    );

    void destroy(VkDevice device);
    void bind(VkCommandBuffer cmd);

    uint32_t vertexCount() const { return count_; }

private:
    VulkanBuffer buffer_;
    uint32_t count_ = 0;

    // staging → device local 的copy
    void copyBuffer(
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        VkBuffer src,
        VkBuffer dst,
        VkDeviceSize size
    );
};

