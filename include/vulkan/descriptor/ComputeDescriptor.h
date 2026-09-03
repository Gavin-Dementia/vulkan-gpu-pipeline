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
        // Shadow pass's light-frustum-culled instance set - see
        // architecture.md's "Shadow mapping" module notes.
        VkBuffer shadowVisibleInstanceBuffer,
        VkBuffer shadowIndirectDrawBuffer,
        VkBuffer lightFrustumBuffer,
        // Hierarchical culling (coarse pass) - see architecture.md.
        VkBuffer clusterBuffer,
        VkBuffer clusterVisibleCameraBuffer,
        VkBuffer clusterVisibleLightBuffer,
        VkDeviceSize objectSize,
        VkDeviceSize visibleInstanceSize,
        VkDeviceSize indirectDrawSize,
        VkDeviceSize frustumSize,
        VkDeviceSize clusterBufferSize,
        VkDeviceSize clusterVisibleSize
    );
  
    void destroy(VkDevice device);

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set()    const { return set_; }

private:
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       set_    = VK_NULL_HANDLE;
};

