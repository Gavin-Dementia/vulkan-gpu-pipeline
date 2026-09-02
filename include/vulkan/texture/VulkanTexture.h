#pragma once
#include <vulkan/vulkan.h>
#include <string>


class VulkanTexture
{
public:
    // format: VK_FORMAT_R8G8B8A8_SRGB for color data (albedo) so the GPU
    // auto-converts sRGB->linear on sample; VK_FORMAT_R8G8B8A8_UNORM for
    // non-color data (normal/metallic-roughness/AO maps) - those must be
    // read back as raw bytes, not gamma-decoded, or the values are wrong.
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        const std::string& path,
        VkFormat format = VK_FORMAT_R8G8B8A8_SRGB
    );

    void destroy(VkDevice device);

    VkImageView view()    const { return view_; }
    VkSampler   sampler() const { return sampler_; }

private:
    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;

    void transitionLayout(
        VkDevice device, VkCommandPool pool, VkQueue queue,
        VkImageLayout oldLayout, VkImageLayout newLayout
    );

    void copyBufferToImage(
        VkDevice device, VkCommandPool pool, VkQueue queue,
        VkBuffer buffer, uint32_t width, uint32_t height
    );

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};

