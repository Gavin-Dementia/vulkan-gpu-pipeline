#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() &&
               presentFamily.has_value();
    }
};

class VulkanDevice {
public:
    static const std::vector<const char*> deviceExtensions;
    void create(VkInstance instance, VkSurfaceKHR surface);
    void destroy();

    VkDevice get() const { return device; }
    VkPhysicalDevice getPhysical() const { return physicalDevice; }
    VkQueue getGraphicsQueue() const { return graphicsQueue; }
    VkQueue getPresentQueue() const { return presentQueue; }
    uint32_t getGraphicsQueueFamilyIndex() const{ return graphicsFamilyIndex; };

private:
    uint32_t graphicsFamilyIndex = UINT32_MAX;
    uint32_t presentFamilyIndex = UINT32_MAX;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSurfaceKHR surface = VK_NULL_HANDLE;

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    bool isDeviceSuitable(
        VkPhysicalDevice device,
        QueueFamilyIndices& outIndices);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);

    void pickPhysicalDevice(VkInstance instance);
    void createLogicalDevice();
};

