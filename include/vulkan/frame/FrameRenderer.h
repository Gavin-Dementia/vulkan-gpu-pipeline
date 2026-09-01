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

private:
    void createSyncObjects();
    void createCommandBuffers();
    void createQueryPools();
};

