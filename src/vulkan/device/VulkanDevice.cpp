#include "vulkan/device/VulkanDevice.h"

#include <vector>
#include <set>
#include <stdexcept>

const std::vector<const char*> VulkanDevice::deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

void VulkanDevice::create(VkInstance instance, VkSurfaceKHR surface)
{
    this->surface = surface;

    pickPhysicalDevice(instance);
    createLogicalDevice();
}

void VulkanDevice::destroy()
{
    if (device)
    {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
}

void VulkanDevice::pickPhysicalDevice(VkInstance instance)
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0)
        throw std::runtime_error("No GPU found");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (auto& dev : devices)
    {
        if (isDeviceSuitable(dev))
        {
            physicalDevice = dev;
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE)
        throw std::runtime_error("No suitable GPU found");
}

bool VulkanDevice::isDeviceSuitable(VkPhysicalDevice dev)
{
    QueueFamilyIndices indices = findQueueFamilies(dev);

    bool extensionsSupported = checkDeviceExtensionSupport(dev);

    return indices.isComplete() && extensionsSupported;
}

QueueFamilyIndices VulkanDevice::findQueueFamilies(VkPhysicalDevice dev)
{
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

    int i = 0;
    for (const auto& f : families)
    {
        if (f.queueFlags & VK_QUEUE_GRAPHICS_BIT)
            indices.graphicsFamily = i;

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &presentSupport);

        if (presentSupport)
            indices.presentFamily = i;

        if (indices.isComplete())
            break;

        i++;
    }

    return indices;
}

bool VulkanDevice::checkDeviceExtensionSupport(VkPhysicalDevice device) 
{
    // uint32_t count;
    // vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

    // std::vector<VkExtensionProperties> available(count);
    // vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    // for (const char* required : deviceExtensions) 
    // {
    //     bool found = false;

    //     for (const auto& ext : available) 
    //     {
    //         if (strcmp(ext.extensionName, required) == 0) 
    //         {
    //             found = true;
    //             break;
    //         }
    //     }

    //     if (!found) return false;
    // }

    return true;
}

void VulkanDevice::createLogicalDevice()
{
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

    graphicsQueueFamilyIndex = indices.graphicsFamily.value();

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

    std::set<uint32_t> uniqueFamilies =
    {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    float priority = 1.0f;

    for (uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;

        queueCreateInfos.push_back(info);
    }

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());

    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &features;

    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());

    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS)
        throw std::runtime_error("Failed to create logical device");

    vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
    vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
}

