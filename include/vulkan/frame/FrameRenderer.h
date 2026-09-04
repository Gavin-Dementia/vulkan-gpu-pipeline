#pragma once

#include <vector>
#include <vulkan/vulkan.h>

#include "vulkan/frame/FrameContext.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/frame/FrameGraph.h"

class FrameRenderer
{
public:
    void init(VulkanContext& context);

    void drawFrame();

    void cleanup();

    uint32_t getImageCount() const
    {  return context->swapchain().getImageViews().size();  }

private:
    VulkanContext* context = nullptr;
    FrameGraph* graph = nullptr;

    std::vector<FrameContext> frames;
    std::vector<VkSemaphore> imageRenderFinished;
    std::vector<VkFence> imagesInFlight;

    uint32_t currentFrame = 0;

    // Monotonic frame count (unlike currentFrame, which wraps) - used to
    // know whether a given frame slot's query pool has ever actually been
    // written to before its first readback attempt (see drawFrame()).
    uint64_t frameCounter_ = 0;

    // Debug-only: shadow map registered with ImGui so its "Shadow Map"
    // window can preview the light-space depth image (Milestone 1 -
    // verifies the shadow pass before any shading code depends on it).
    VkDescriptorSet shadowMapDebugSet_ = VK_NULL_HANDLE;

    // Scene color target registered with ImGui so the dockable "Viewport"
    // window can display the live 3D scene (see PassStage::UI).
    VkDescriptorSet sceneViewportSet_ = VK_NULL_HANDLE;

    // One-time default dock layout (Viewport centered, debug windows
    // docked to the right) - applied only on the first UI-stage frame so
    // a manually-rearranged layout isn't stomped on every frame after.
    bool dockLayoutInitialized_ = false;

    bool     resizePending_ = false;
    uint32_t pendingWidth_  = 0;
    uint32_t pendingHeight_ = 0;

    // Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39) -
    // set when vkAcquireNextImageKHR/vkQueuePresentKHR report the
    // swapchain is suboptimal/out of date, checked (alongside a direct
    // framebuffer-size comparison) at the top of the next drawFrame().
    bool swapchainNeedsRecreate_ = false;

private:
    void createSyncObjects();
    void createCommandBuffers();
    void createQueryPools();

    // Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39) -
    // recreates the swapchain-derived resources FrameRenderer itself owns
    // per swapchain image (imagesInFlight/imageRenderFinished) after
    // VulkanContext::resizeSwapchain() runs, and tells ImGui's Vulkan
    // backend about a changed image count.
    void recreateSwapchainResources();

    // drawFrame() split (roadmap.md's "Refactor backlog" #1) - each
    // record*() records exactly one render pass (or, for
    // readbackFrameStats(), one CPU-side readback group) into the
    // command buffer, in the same place and with the same behavior as
    // when this was all inline in drawFrame(). Pure mechanical
    // extraction, no logic change - drawFrame() itself still owns
    // ordering, the GPU-timestamp writes between passes, and command
    // buffer begin/end/submit/present.
    void readbackFrameStats(const FrameContext& frame);
    void recordComputePass(VkCommandBuffer cmd);
    void recordShadowPass(VkCommandBuffer cmd);
    void recordRefractionCopy(VkCommandBuffer cmd);
    void recordScenePass(VkCommandBuffer cmd);
    void recordUIPass(VkCommandBuffer cmd, uint32_t imageIndex);
};

