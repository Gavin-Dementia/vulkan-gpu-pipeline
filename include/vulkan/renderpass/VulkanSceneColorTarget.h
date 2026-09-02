#pragma once
#include <vulkan/vulkan.h>

// Fixed-resolution offscreen color target the 3D scene renders into,
// sampled by ImGui's dockable "Viewport" panel (see FrameRenderer.cpp) -
// same "sampled render target, not the swapchain's own attachment" idea
// as VulkanShadowMap, just a color image instead of depth. Fixed size
// rather than resized to match the panel: this codebase has no swapchain
// resize handling either (Camera::ASPECT_RATIO is a fixed constant), so a
// live-resized viewport would need new plumbing this class deliberately
// doesn't add yet.
class VulkanSceneColorTarget
{
public:
    static constexpr uint32_t WIDTH  = 1280;
    static constexpr uint32_t HEIGHT = 1024;
    static constexpr VkFormat FORMAT = VK_FORMAT_B8G8R8A8_SRGB;   // matches the swapchain's format

    void create(VkPhysicalDevice physical, VkDevice device);
    void destroy(VkDevice device);

    VkImage     image()  const { return image_; }
    VkImageView view()   const { return view_; }
    VkExtent2D  extent() const { return { WIDTH, HEIGHT }; }

private:
    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};
