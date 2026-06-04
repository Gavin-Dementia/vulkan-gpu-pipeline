#pragma once

#include <vulkan/vulkan.h>
#include <vector>

class VulkanContext {
public:
    void init();
    void cleanup();

private:
    VkInstance instance;

    void printPhysicalDevices();
    bool checkValidationLayerSupport();
    std::vector<const char*> getRequiredExtensions();
};

