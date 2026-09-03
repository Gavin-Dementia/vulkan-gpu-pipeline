#pragma once
#include <vulkan/vulkan.h>
#include <array>

// A 6-layer cube-compatible image: one VK_IMAGE_VIEW_TYPE_CUBE view for
// sampling (samplerCube in a shader) plus 6 individual VK_IMAGE_VIEW_TYPE_2D
// views (one per array layer) for use as render-pass color attachments when
// baking content into the cube one face at a time - Vulkan render passes
// attach 2D image views, not cube views, per subresource. Single mip level
// only (see docs/TECHNICAL_NOTES.md - IBL Milestone 1): a later milestone
// baking a prefiltered specular mip chain will need to extend this, not
// needed yet for a flat environment/irradiance cubemap.
class VulkanCubemap
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        uint32_t faceSize,
        VkFormat format,
        VkImageUsageFlags usage
    );

    void destroy(VkDevice device);

    VkImage     image()    const { return image_; }
    VkImageView cubeView() const { return cubeView_; }
    VkImageView faceView(int i) const { return faceViews_[i]; }
    VkSampler   sampler()  const { return sampler_; }
    uint32_t    faceSize() const { return faceSize_; }

private:
    VkImage        image_    = VK_NULL_HANDLE;
    VkDeviceMemory memory_   = VK_NULL_HANDLE;
    VkImageView    cubeView_ = VK_NULL_HANDLE;
    std::array<VkImageView, 6> faceViews_{};
    VkSampler      sampler_  = VK_NULL_HANDLE;
    uint32_t       faceSize_ = 0;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};
