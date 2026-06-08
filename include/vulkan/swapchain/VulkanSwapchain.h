#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <vector>

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanSwapchain {
public:
    void create(VkPhysicalDevice physicalDevice,
                VkDevice device,
                VkSurfaceKHR surface,
                GLFWwindow* window);

    void destroy(VkDevice device);
    
    VkFormat getFormat() const { return imageFormat; }
    const std::vector<VkImage>& getImages() const { return images; }
    VkExtent2D getExtent() const { return extent; }
    VkSwapchainKHR get() const { return swapchain; }
    
private:
    VkFormat imageFormat;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    VkExtent2D extent;

    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;

    SwapchainSupportDetails querySupport(VkPhysicalDevice device, VkSurfaceKHR surface);

    VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                            GLFWwindow* window);
};

