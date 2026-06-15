#include "vulkan/buffer/UniformBuffer.h"
#include <cstring>

// Uniform Buffer ||  Vertex Buffer
// Vertex Buffer  : upload vertex data once, read while render each frame
// Uniform Buffer : matirx , CPU write each frame, shader read
// Uniform Buffer use HOST_VISIBLE 
// cause each frame written by CPU use staging will become slower
void UniformBuffer::create(VkPhysicalDevice physical, VkDevice device)
{
    // HOST_VISIBLE + HOST_COHERENT：CPU writ, no need staging
    buffer_.create(
        physical, device,
        sizeof(UBOData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
}

void UniformBuffer::update(VkDevice device, const UBOData& data)
{
    buffer_.upload(device, &data, sizeof(UBOData));
}

void UniformBuffer::destroy(VkDevice device)
{
    buffer_.destroy(device);
}

