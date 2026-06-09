#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <vector>

struct SwapchainSupportDetails 
{
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanSwapchain 
{
public:
    void create(VkPhysicalDevice physicalDevice,
                VkDevice device,
                VkSurfaceKHR surface,
                GLFWwindow* window);

    void destroy(VkDevice device);
    void createImageViews(VkDevice device);

    VkSwapchainKHR getSwapchain() const
    {  return swapchain;  }

    const std::vector<VkImageView>& getImageViews() const
    {  return imageViews;  }

    VkExtent2D getExtent() const
    {  return extent;  }

    VkFormat getImageFormat() const
    {  return imageFormat;  }

private:

    VkFormat imageFormat{};
    VkExtent2D extent{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;

    SwapchainSupportDetails querySupport(VkPhysicalDevice device, VkSurfaceKHR surface);

    VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                            GLFWwindow* window);
};

