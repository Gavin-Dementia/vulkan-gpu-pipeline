#pragma once
#include <vulkan/vulkan.h>

// The BRDF integration LUT (IBL Milestone 3 - see docs/TECHNICAL_NOTES.md
// §35): a single 2D image storing Karis's precomputed split-sum second
// integral, indexed by (NdotV, roughness). One VK_IMAGE_VIEW_TYPE_2D view
// serves both roles - the one-shot bake's render-pass color attachment,
// and later the sampler2D a shader reads - unlike VulkanCubemap there's
// only ever one "face", so no cube-view/face-view split is needed.
class VulkanBRDFLut
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        uint32_t size,
        VkFormat format,
        VkImageUsageFlags usage
    );

    void destroy(VkDevice device);

    VkImage     image()  const { return image_; }
    VkImageView view()   const { return view_; }
    VkSampler   sampler() const { return sampler_; }
    uint32_t    size()   const { return size_; }

private:
    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;
    uint32_t       size_   = 0;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};
