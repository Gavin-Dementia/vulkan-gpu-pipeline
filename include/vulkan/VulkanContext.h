#pragma once
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include "vulkan/command/VulkanCommandPool.h"
#include "vulkan/core/VulkanInstance.h"
#include "vulkan/device/VulkanDevice.h"
#include "vulkan/platform/VulkanSurface.h"
#include "vulkan/render/VulkanFramebuffer.h"
#include "vulkan/render/VulkanRenderPass.h"
#include "vulkan/swapchain/VulkanImageView.h"
#include "vulkan/swapchain/VulkanSwapchain.h"
#include "vulkan/sync/SyncSystem.h"

class VulkanContext {
public:
    void init(GLFWwindow* window);
    void cleanup();
    void createCommandBuffer();
    void drawFrame();
    void setupSyncObjects(VkDevice device);

private:
    VulkanInstance instance;
    VulkanSurface surface;
    VulkanDevice device;
    VulkanSwapchain swapchain;
    VulkanImageView imageView;
    VulkanRenderPass renderPass;
    VulkanFramebuffer framebuffer;
    VulkanCommandPool commandPool;
    SyncSystem sync;
    uint32_t frameIndex = 0;

    //vulkan
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
    VkFence inFlightFence;
};

