#include "vulkan/texture/Material.h"

void Material::load(
    VkPhysicalDevice physical,
    VkDevice device,
    VkCommandPool pool,
    VkQueue queue,
    const std::string& albedoPath,
    const std::string& normalPath,
    const std::string& metallicRoughnessPath,
    const std::string& aoPath)
{
    albedo_.create(physical, device, pool, queue, albedoPath, VK_FORMAT_R8G8B8A8_SRGB);
    normal_.create(physical, device, pool, queue, normalPath, VK_FORMAT_R8G8B8A8_UNORM);
    metallicRoughness_.create(physical, device, pool, queue, metallicRoughnessPath, VK_FORMAT_R8G8B8A8_UNORM);
    ao_.create(physical, device, pool, queue, aoPath, VK_FORMAT_R8G8B8A8_UNORM);
}

void Material::destroy(VkDevice device)
{
    albedo_.destroy(device);
    normal_.destroy(device);
    metallicRoughness_.destroy(device);
    ao_.destroy(device);
}
