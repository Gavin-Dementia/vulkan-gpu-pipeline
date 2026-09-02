#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "vulkan/texture/Material.h"

class VulkanDescriptor
{
public:
    void create(
        VkDevice device,
        VkBuffer uniformBuffer,
        Material& material,
        VkBuffer sceneDataBuffer,
        VkImageView shadowMapView,
        VkSampler shadowMapSampler
    );
    void destroy(VkDevice device);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set()    const { return set_; }

private:
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       set_    = VK_NULL_HANDLE;

    void createLayout(VkDevice device);
    void createPool(VkDevice device);
    void allocateAndWrite(
        VkDevice device,
        VkBuffer uniformBuffer,
        Material& material,
        VkBuffer sceneDataBuffer,
        VkImageView shadowMapView,
        VkSampler shadowMapSampler
    );
};

