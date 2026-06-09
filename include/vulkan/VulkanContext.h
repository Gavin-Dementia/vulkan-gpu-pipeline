#pragma once
#include <GLFW/glfw3.h>

#include "vulkan/core/VulkanInstance.h"
#include "vulkan/platform/VulkanSurface.h"
#include "vulkan/device/VulkanDevice.h"
#include "vulkan/swapchain/VulkanSwapchain.h"


class VulkanContext {
public:
    void init(GLFWwindow* window);
    void cleanup();

private:
    VulkanInstance instance;
    VulkanSurface surface;
    VulkanDevice device;
    VulkanSwapchain swapchain;
};

