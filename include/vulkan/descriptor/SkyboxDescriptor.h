#pragma once
#include <vulkan/vulkan.h>

// Tiny descriptor set for the live skybox draw - one binding, the
// environment cubemap's sampler. Sibling to VulkanDescriptor/
// ComputeDescriptor, kept as its own small class rather than folding
// into VulkanDescriptor since the skybox pipeline's layout has nothing
// else in common with the grid/projectile's 7-binding material set.
class SkyboxDescriptor
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
