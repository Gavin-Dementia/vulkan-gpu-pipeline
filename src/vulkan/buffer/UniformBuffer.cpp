#include "vulkan/buffer/UniformBuffer.h"
#include <cstring>

void UniformBuffer::create(VkPhysicalDevice physical, VkDevice device)
{
    // HOST_VISIBLE + HOST_COHERENT：CPU 直接写，不需要 staging
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

