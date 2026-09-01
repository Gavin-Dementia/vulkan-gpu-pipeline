#pragma once

#include <vulkan/vulkan.h>

struct FrameContext
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;

    VkFence inFlightFence = VK_NULL_HANDLE;

    // GPU timestamp queries for this frame slot (4 slots: frame start,
    // compute end, shadow end, graphics end - see FrameRenderer::drawFrame()).
    // VK_NULL_HANDLE if the GPU doesn't support timestamp queries.
    VkQueryPool queryPool = VK_NULL_HANDLE;
};

