#include "vulkan/swapchain/VulkanSwapchain.h"
#include <exception>
#include <iostream>

SwapchainSupportDetails VulkanSwapchain::querySupport(
    VkPhysicalDevice device,
    VkSurfaceKHR surface) {

    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentCount, nullptr);

    if (presentCount != 0) {
        details.presentModes.resize(presentCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanSwapchain::chooseFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) {

    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }

    return formats[0];
}

VkPresentModeKHR VulkanSwapchain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) {

    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::chooseExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    GLFWwindow* window) {

    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }

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
    GLFWwindow* window) {

    auto support = querySupport(physicalDevice, surface);

    VkSurfaceFormatKHR chosenFormat = chooseFormat(support.formats);
    imageFormat = chosenFormat.format;

    VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);

    VkExtent2D chosenExtent;
    if (support.capabilities.currentExtent.width != UINT32_MAX) {
        chosenExtent = support.capabilities.currentExtent;
    } else {
        chosenExtent = chooseExtent(support.capabilities, window);
    }
    extent = chosenExtent;

    uint32_t imageCount = support.capabilities.minImageCount + 1;

    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;

    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosenFormat.format;
    createInfo.imageColorSpace = chosenFormat.colorSpace;
    createInfo.imageExtent = chosenExtent;
    createInfo.imageArrayLayers = 1;

    // ✔ render target usage
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;    
    
    // --------------------------------------------------------
    // queue family sharing mode
    // --------------------------------------------------------

    uint32_t queueFamilyIndices[] = {
        // graphicsFamily
        0,
        // presentFamily
        0
    };

    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices = nullptr;

    // --------------------------------------------------------
    // transform / composite / present
    // --------------------------------------------------------

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    createInfo.oldSwapchain = VK_NULL_HANDLE;

    // --------------------------------------------------------
    // create swapchain
    // --------------------------------------------------------

    if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

    // --------------------------------------------------------
    // get images
    // --------------------------------------------------------

    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);

    images.resize(imageCount);

    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());

    std::cout << "[Vulkan] Swapchain created successfully\n";
    std::cout << "Image count: " << imageCount << "\n";
}

void VulkanSwapchain::destroy(VkDevice device) {

    for (auto view : imageViews) {
        vkDestroyImageView(device, view, nullptr);
    }

    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
}



