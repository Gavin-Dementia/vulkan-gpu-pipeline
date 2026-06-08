#include "vulkan/sync/SyncSystem.h"
#include <stdexcept>

void SyncSystem::create(VkDevice device_, uint32_t maxFramesInFlight, uint32_t imageCount)
{
    device = device_;

    frames.resize(maxFramesInFlight);
    imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& f : frames) {
        if (vkCreateSemaphore(device, &sem, nullptr, &f.imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(device, &sem, nullptr, &f.renderFinished) != VK_SUCCESS ||
            vkCreateFence(device, &fence, nullptr, &f.inFlightFence) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create sync objects");
        }
    }
}

void SyncSystem::destroy(VkDevice device) 
{
    for (auto& frame : frames) {
        vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        vkDestroySemaphore(device, frame.renderFinished, nullptr);
        vkDestroyFence(device, frame.inFlightFence, nullptr);
    }

    for (auto& f : imagesInFlight) {
        if (f != VK_NULL_HANDLE) {
            vkDestroyFence(device, f, nullptr);
        }
    }
}

FrameSync& SyncSystem::getCurrentFrame() 
{    return frames[currentFrame];  }

VkFence& SyncSystem::getImageFence(uint32_t imageIndex)
{    return imagesInFlight[imageIndex];  }

void SyncSystem::advance() 
{    currentFrame = (currentFrame + 1) % frames.size();  }

