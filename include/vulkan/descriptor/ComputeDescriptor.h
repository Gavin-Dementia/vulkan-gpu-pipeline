#pragma once
#include <vulkan/vulkan.h>
#include <array>

class ComputeDescriptor
{
public:  
    void create(
        VkDevice device,
        VkBuffer objectBuffer,
        std::array<VkBuffer, 3> visibleInstanceBuffers,
        std::array<VkBuffer, 3> indirectDrawBuffers,
        VkBuffer frustumBuffer,
        VkDeviceSize objectSize,
        VkDeviceSize visibleInstanceSize,
        VkDeviceSize indirectDrawSize,
        VkDeviceSize frustumSize
    );
  
    void destroy(VkDevice device);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set()    const { return set_; }

private:
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       set_    = VK_NULL_HANDLE;
};

