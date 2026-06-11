#pragma once

#include <GLFW/glfw3.h>

#include "vulkan/command/VulkanCommandPool.h"
#include "vulkan/core/VulkanInstance.h"
#include "vulkan/device/VulkanDevice.h"
#include "vulkan/platform/VulkanSurface.h"
#include "vulkan/renderpass/VulkanFramebuffer.h"
#include "vulkan/renderpass/VulkanRenderPass.h"
#include "vulkan/swapchain/VulkanSwapchain.h"
#include "vulkan/pipeline/VulkanPipeline.h"


class VulkanContext
{
public:
    void init(GLFWwindow* window);
    void cleanup();

    VulkanDevice& device() { return device_; }
    VulkanSwapchain& swapchain() { return swapchain_; }
    VulkanInstance& instance() { return instance_; }
    VulkanSurface& surface() { return surface_; }
    VulkanCommandPool& commandPool() { return commandPool_; }
    VulkanRenderPass& renderPass() { return renderPass_; }
    VulkanFramebuffer& framebuffer() { return framebuffer_; }
    VulkanPipeline& pipeline() { return pipeline_; }

    GLFWwindow* window() { return window_; }

private:
    GLFWwindow* window_ = nullptr;

    VulkanInstance instance_;
    VulkanSurface surface_;
    VulkanDevice device_;
    VulkanSwapchain swapchain_;
    VulkanCommandPool commandPool_;
    VulkanRenderPass renderPass_;
    VulkanFramebuffer framebuffer_;
    VulkanPipeline pipeline_;
};

