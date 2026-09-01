#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const
    {
        return graphicsFamily.has_value() &&
               presentFamily.has_value();
    }
};

class VulkanDevice
{
public:
    static const std::vector<const char*> deviceExtensions;

    void create(VkInstance instance, VkSurfaceKHR surface);
    void destroy();

    VkDevice get() const {  return device;  }

    VkPhysicalDevice getPhysical() const {  return physicalDevice;  }

    VkQueue getGraphicsQueue() const {  return graphicsQueue;  }

    VkQueue getPresentQueue() const {  return presentQueue;  }

    VkSurfaceKHR getSurface() const {  return surface;  }

    uint32_t getGraphicsQueueFamily() const
    {  return graphicsQueueFamilyIndex; }

    // Nanoseconds per timestamp tick (VkPhysicalDeviceLimits::timestampPeriod)
    // and whether the graphics queue supports vkCmdWriteTimestamp at all -
    // both queried once in create(), not assumed, since not every GPU
    // supports timestamp queries on every queue family.
    float timestampPeriodNs() const     { return timestampPeriodNs_; }
    bool  supportsTimestampQueries() const { return supportsTimestampQueries_; }

private:
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSurfaceKHR surface = VK_NULL_HANDLE;

    uint32_t graphicsQueueFamilyIndex = 0;

    float timestampPeriodNs_ = 0.0f;
    bool  supportsTimestampQueries_ = false;

private:
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);

    bool isDeviceSuitable(VkPhysicalDevice device);

    bool checkDeviceExtensionSupport(VkPhysicalDevice device);

    void pickPhysicalDevice(VkInstance instance);

    void createLogicalDevice();

    void queryTimestampSupport();
};