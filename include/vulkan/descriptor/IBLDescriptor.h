#pragma once
#include <vulkan/vulkan.h>

// The main graphics pipeline's set 1 - ambient-lighting data, shared
// globally by every material rather than living in the per-object set 0
// (VulkanDescriptor). 3 bindings: irradiance cubemap (diffuse, IBL
// Milestone 2), prefiltered specular cubemap + BRDF LUT (specular, IBL
// Milestone 3) - see docs/TECHNICAL_NOTES.md §34/§35. Replaces the
// single-binding CubeSamplerDescriptor M2 used here; CubeSamplerDescriptor
// itself is untouched and keeps its other uses (skybox draw, bake-pass
// inputs) unchanged.
class IBLDescriptor
{
public:
    void create(
        VkDevice device,
        VkImageView irradianceCubeView, VkSampler irradianceSampler,
        VkImageView prefilteredCubeView, VkSampler prefilteredSampler,
        VkImageView brdfLutView, VkSampler brdfLutSampler
    );

    void destroy(VkDevice device);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set()    const { return set_; }

private:
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       set_    = VK_NULL_HANDLE;
};
