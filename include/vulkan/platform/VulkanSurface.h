#pragma once
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

class VulkanSurface {
public:
    void create(VkInstance instance, GLFWwindow* window);
    void destroy(VkInstance instance);

    VkSurfaceKHR get() const { return surface; }

private:
    VkSurfaceKHR surface = VK_NULL_HANDLE;
};

