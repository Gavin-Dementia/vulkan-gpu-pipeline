#pragma once

#include <vulkan/vulkan.h>

struct FrameContext
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;

    VkFence inFlightFence = VK_NULL_HANDLE;
};

