#pragma once
#include <vulkan/vulkan.h>

// Offscreen color target the 3D scene renders into, sampled by ImGui's
// dockable "Viewport" panel (see FrameRenderer.cpp) - same "sampled
// render target, not the swapchain's own attachment" idea as
// VulkanShadowMap, just a color image instead of depth. Resizable as of
// docs/TECHNICAL_NOTES.md §36: create() takes width/height as runtime
// parameters (not compile-time constants), and VulkanContext::
// resizeSceneTarget() destroys+recreates this (plus sceneColorDepth_/
// sceneFramebuffer_/pipeline_/skyboxPipeline_) at a new size whenever
// the docked Viewport panel is resized - see that method and §36 for the
// full design (why vkDeviceWaitIdle, why applied at the top of the next
// frame rather than immediately).
class VulkanSceneColorTarget
{
public:
    static constexpr VkFormat FORMAT = VK_FORMAT_B8G8R8A8_SRGB;   // matches the swapchain's format

    void create(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height);
    void destroy(VkDevice device);

    VkImage     image()  const { return image_; }
    VkImageView view()   const { return view_; }
    VkExtent2D  extent() const { return { width_, height_ }; }

private:
    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
    uint32_t       width_  = 0;
    uint32_t       height_ = 0;

    uint32_t findMemoryType(
        VkPhysicalDevice physical,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties
    );
};
