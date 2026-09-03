#pragma once
#include <vulkan/vulkan.h>

// A single combined-image-sampler descriptor set bound to one cubemap -
// generic, not tied to any one use site. Three uses as of IBL Milestone 2
// (see docs/TECHNICAL_NOTES.md §34): the live skybox draw (bound to
// environmentCubemap_), the irradiance-convolution bake's input (same
// instance, same environmentCubemap_ binding, reused as-is), and the main
// graphics pipeline's new ambient-lighting descriptor set (a second
// instance, bound to irradianceCubemap_). Sibling to VulkanDescriptor/
// ComputeDescriptor, kept as its own small class since its layout has
// nothing in common with the grid/projectile's 7-binding material set.
class CubeSamplerDescriptor
{
public:
    void create(
        VkDevice device,
        VkImageView cubeView,
        VkSampler cubeSampler
    );

    void destroy(VkDevice device);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set()    const { return set_; }

private:
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       set_    = VK_NULL_HANDLE;
};
