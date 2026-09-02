#pragma once
#include "vulkan/texture/VulkanTexture.h"
#include <string>

// Bundles the 4 textures a PBR material samples (Phase 8 milestone 2 -
// see docs/TECHNICAL_NOTES.md). Grid and projectile currently share one
// Material instance, same "reuses the single shared texture" reasoning
// VulkanContext already used for the single-texture design (see
// docs/architecture.md's Lighting module notes) - a distinct per-object
// Material is future work, not needed until materials actually diverge.
class Material
{
public:
    void load(
        VkPhysicalDevice physical,
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        const std::string& albedoPath,
        const std::string& normalPath,
        const std::string& metallicRoughnessPath,
        const std::string& aoPath
    );

    void destroy(VkDevice device);

    VulkanTexture& albedo()            { return albedo_; }
    VulkanTexture& normal()            { return normal_; }
    VulkanTexture& metallicRoughness() { return metallicRoughness_; }
    VulkanTexture& ao()                { return ao_; }

private:
    VulkanTexture albedo_;             // VK_FORMAT_R8G8B8A8_SRGB - color data
    VulkanTexture normal_;             // VK_FORMAT_R8G8B8A8_UNORM - not color data
    VulkanTexture metallicRoughness_;  // VK_FORMAT_R8G8B8A8_UNORM - glTF convention: G=roughness, B=metallic
    VulkanTexture ao_;                 // VK_FORMAT_R8G8B8A8_UNORM
};
