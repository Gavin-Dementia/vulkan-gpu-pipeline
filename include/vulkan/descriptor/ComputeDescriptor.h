#pragma once
#include <vulkan/vulkan.h>

class ComputeDescriptor
{
public:
    void create(
        VkDevice device,
        VkBuffer objectBuffer,
        VkBuffer visibleInstanceBuffer,
        VkBuffer frustumBuffer,
        VkBuffer indirectDrawBuffer,
        VkDeviceSize objectBufferSize,
        VkDeviceSize visibleInstanceBufferSize,
        VkDeviceSize frustumBufferSize,
        VkDeviceSize indirectDrawBufferSize
    );
    void destroy(VkDevice device);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set()    const { return set_; }

private:
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       set_    = VK_NULL_HANDLE;
};

