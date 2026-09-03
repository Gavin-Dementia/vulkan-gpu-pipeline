#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <vector>

// A cube-compatible image, 1+ mip levels: one VK_IMAGE_VIEW_TYPE_CUBE
// view spanning every mip (for sampling as samplerCube, with mip
// selection via textureLod) plus, per mip level, 6 individual
// VK_IMAGE_VIEW_TYPE_2D views (one per array layer) for use as
// render-pass color attachments when baking content into the cube one
// (mip, face) at a time - Vulkan render passes attach 2D image views,
// not cube views, per subresource. mipLevels defaults to 1 (the shape
// environmentCubemap_/irradianceCubemap_ still use, IBL Milestone 1/2) -
// IBL Milestone 3's prefilteredCubemap_ is the first user of mipLevels>1,
// see docs/TECHNICAL_NOTES.md §35.
class VulkanCubemap
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        uint32_t faceSize,
        VkFormat format,
        VkImageUsageFlags usage,
        uint32_t mipLevels = 1
    );

    void destroy(VkDevice device);

    VkImage     image()    const { return image_; }
    VkImageView cubeView() const { return cubeView_; }
    VkImageView faceView(uint32_t face, uint32_t mip = 0) const { return faceViews_[mip][face]; }
    VkSampler   sampler()  const { return sampler_; }
    uint32_t    faceSize() const { return faceSize_; }
    uint32_t    mipLevels() const { return mipLevels_; }

private:
    VkImage        image_    = VK_NULL_HANDLE;
    VkDeviceMemory memory_   = VK_NULL_HANDLE;
    VkImageView    cubeView_ = VK_NULL_HANDLE;
    std::vector<std::array<VkImageView, 6>> faceViews_;   // [mip][face]
    VkSampler      sampler_  = VK_NULL_HANDLE;
    uint32_t       faceSize_ = 0;
    uint32_t       mipLevels_ = 1;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};
