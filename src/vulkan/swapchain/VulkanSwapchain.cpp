#include "vulkan/swapchain/VulkanSwapchain.h"
#include <exception>
#include <iostream>

SwapchainSupportDetails VulkanSwapchain::querySupport(
    VkPhysicalDevice device,
    VkSurfaceKHR surface) 
{
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

    if (formatCount != 0) 
    {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentCount, nullptr);

    if (presentCount != 0) 
    {
        details.presentModes.resize(presentCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanSwapchain::chooseFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) 
{
    for (const auto& format : formats) 
    {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) 
        {
            return format;
        }
    }

    return formats[0];
}

VkPresentModeKHR VulkanSwapchain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) 
{
    for (const auto& mode : modes) 
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) 
        {
            return mode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::chooseExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    GLFWwindow* window) 
{
    if (capabilities.currentExtent.width != UINT32_MAX) 
    {    return capabilities.currentExtent;  }

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    VkExtent2D actualExtent = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };

    actualExtent.width = std::max(capabilities.minImageExtent.width,
                                  std::min(capabilities.maxImageExtent.width, actualExtent.width));

    actualExtent.height = std::max(capabilities.minImageExtent.height,
                                   std::min(capabilities.maxImageExtent.height, actualExtent.height));

    return actualExtent;
}

void VulkanSwapchain::create(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window)
{
    auto support = querySupport(physicalDevice, surface);

    VkSurfaceFormatKHR format = chooseFormat(support.formats);
    VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);

    imageFormat = format.format;

    VkExtent2D ext;
    if (support.capabilities.currentExtent.width != UINT32_MAX)
        ext = support.capabilities.currentExtent;
    else
        ext = chooseExtent(support.capabilities, window);

    extent = ext;

    uint32_t imageCount = support.capabilities.minImageCount + 1;

    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount)
    {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;

    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = ext;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = support.capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(device, &info, nullptr, &swapchain) != VK_SUCCESS)
        throw std::runtime_error("Failed to create swapchain");

    // =========================
    // 1. GET IMAGES FIRST
    // =========================
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);

    images.resize(imageCount);

    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());

    // =========================
    // 2. THEN CREATE IMAGE VIEWS
    // =========================
    createImageViews(device);

    std::cout << "[Vulkan] Swapchain created successfully\n";
    std::cout << "Image count: " << imageCount << "\n";
}

void VulkanSwapchain::createImageViews(VkDevice device)
{
    imageViews.resize(images.size());

    for (size_t i = 0; i < images.size(); i++)
    {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = images[i];

        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = imageFormat;

        info.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY
        };

        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &info, nullptr, &imageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create image view");
    }
}

void VulkanSwapchain::destroy(VkDevice device)
{
    for (auto view : imageViews)
        vkDestroyImageView(device, view, nullptr);

    imageViews.clear();
    images.clear();

    if (swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

