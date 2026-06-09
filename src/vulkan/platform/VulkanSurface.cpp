#include "vulkan/platform/VulkanSurface.h"

#include <iostream>
#include <stdexcept>

#include <GLFW/glfw3.h>

// ------------------------------------------------------------
// Create Surface
// ------------------------------------------------------------

void VulkanSurface::create(VkInstance instance, GLFWwindow* window) 
{

    if (!window) 
    {    throw std::runtime_error("GLFW window is null");  }

    VkResult result = glfwCreateWindowSurface(
        instance,
        window,
        nullptr,
        &surface
    );

    if (result != VK_SUCCESS) 
    {    throw std::runtime_error("Failed to create Vulkan surface");  }

    std::cout << "[Vulkan] Surface created successfully\n";
}

// ------------------------------------------------------------
// Destroy Surface
// ------------------------------------------------------------

void VulkanSurface::destroy(VkInstance instance) 
{

    if (surface != VK_NULL_HANDLE) 
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
}

