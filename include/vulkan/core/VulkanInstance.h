#pragma once
#include <vulkan/vulkan.h>
#include <vector>

class VulkanInstance 
{
public:
    void create();
    void destroy();

    VkInstance get() const { return instance; }

private:
    VkInstance instance = VK_NULL_HANDLE;

    std::vector<const char*> getExtensions();
    bool checkValidationLayers();
};

