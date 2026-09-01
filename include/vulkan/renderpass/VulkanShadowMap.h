#pragma once
#include <vulkan/vulkan.h>

// Depth-only render target sampled by the main fragment shader (unlike
// VulkanDepthBuffer, which backs the swapchain's depth attachment and is
// never sampled - see architecture.md). Fixed resolution, independent of
// swapchain size, since it's rendered from the light's point of view.
class VulkanShadowMap
{
public:
    static constexpr uint32_t RESOLUTION = 2048;
    static constexpr VkFormat FORMAT     = VK_FORMAT_D32_SFLOAT;

    void create(VkPhysicalDevice physical, VkDevice device);
    void destroy(VkDevice device);

    VkImage     image()   const { return image_; }
    VkImageView view()    const { return view_; }
    VkSampler   sampler() const { return sampler_; }
    VkExtent2D  extent()  const { return { RESOLUTION, RESOLUTION }; }

private:
    VkImage        image_   = VK_NULL_HANDLE;
    VkDeviceMemory memory_  = VK_NULL_HANDLE;
    VkImageView    view_    = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};
