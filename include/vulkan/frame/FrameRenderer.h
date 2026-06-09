#pragma once

#include <vector>
#include <vulkan/vulkan.h>

#include "vulkan/frame/FrameContext.h"
#include "vulkan/VulkanContext.h"

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

    std::vector<FrameContext> frames;
    std::vector<VkFence> imagesInFlight;

    uint32_t currentFrame = 0;

private:
    void createSyncObjects();
    void createCommandBuffers();
};

