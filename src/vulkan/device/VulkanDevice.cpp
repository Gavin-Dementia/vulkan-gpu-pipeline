#include "vulkan/device/VulkanDevice.h"
#include <iostream>

const std::vector<const char*> VulkanDevice::deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

void VulkanDevice::create(VkInstance instance, VkSurfaceKHR surface_) {
    surface = surface_;
    pickPhysicalDevice(instance);
    createLogicalDevice();
}

void VulkanDevice::pickPhysicalDevice(VkInstance instance) {

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        throw std::runtime_error("No GPU found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const auto& dev : devices) {

        QueueFamilyIndices indices = findQueueFamilies(dev);

        if (isDeviceSuitable(dev, indices)) {

            physicalDevice = dev;

            graphicsFamilyIndex = indices.graphicsFamily.value();
            presentFamilyIndex   = indices.presentFamily.value();

            std::cout << "[Vulkan] Selected physical device\n";
            return;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("No suitable GPU found");
    }
}

bool VulkanDevice::isDeviceSuitable(
    VkPhysicalDevice device,
    QueueFamilyIndices& outIndices)
{
    QueueFamilyIndices indices = findQueueFamilies(device);

    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool valid = indices.isComplete() && extensionsSupported;

    if (valid) {
        outIndices = indices;
    }

    return valid;
}

QueueFamilyIndices VulkanDevice::findQueueFamilies(VkPhysicalDevice device) {

    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    int i = 0;
    for (const auto& f : families) {

        if (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(
            device, i, surface, &presentSupport);

        if (presentSupport) {
            indices.presentFamily = i;
        }

        if (indices.isComplete()) break;

        i++;
    }

    return indices;
}

bool VulkanDevice::checkDeviceExtensionSupport(VkPhysicalDevice device) {

    uint32_t count;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    for (const char* required : deviceExtensions) {

        bool found = false;

        for (const auto& ext : available) {
            if (strcmp(ext.extensionName, required) == 0) {
                found = true;
                break;
            }
        }

        if (!found) return false;
    }

    return true;
}

void VulkanDevice::createLogicalDevice() {

    QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

    float priority = 1.0f;

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = indices.graphicsFamily.value();
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.queueCreateInfoCount = 1;

    createInfo.pEnabledFeatures = &features;

    // ======================================================
    // enable swapchain extension
    // ======================================================
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());

    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);

    if (indices.presentFamily.has_value()) {
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    }

    std::cout << "[Vulkan] Logical device created\n";
}


void VulkanDevice::destroy() {
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
}

