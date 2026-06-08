#pragma once
#include <vulkan/vulkan.h>
#include <vector>

struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
};

class SyncSystem {
public:
    void create(VkDevice device, uint32_t maxFramesInFlight, uint32_t imageCount);
    void destroy(VkDevice device);

    FrameSync& getCurrentFrame();
    VkFence& getImageFence(uint32_t imageIndex);

    void advance();

private:
    VkDevice device{};

    std::vector<FrameSync> frames;
    std::vector<VkFence> imagesInFlight;

    uint32_t currentFrame = 0;
};


