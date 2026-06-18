#pragma once
#include <vulkan/vulkan.h>
#include <string>


class VulkanTexture
{
public:
    void create(
        VkPhysicalDevice physical,
        VkDevice device,
        VkCommandPool pool,
        VkQueue queue,
        const std::string& path
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

