#include "vulkan/core/VulkanInstance.h"

#include <iostream>
#include <stdexcept>
#include <cstring>
#include <GLFW/glfw3.h>
#include <vector>

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

static const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

// ------------------------------------------------------------
// Extensions
// ------------------------------------------------------------

std::vector<const char*> VulkanInstance::getExtensions() 
{

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions,
                                         glfwExtensions + glfwExtensionCount);

    if (enableValidationLayers) 
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

// ------------------------------------------------------------
// Validation layer support check
// ------------------------------------------------------------

bool VulkanInstance::checkValidationLayers() 
{

    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : validationLayers) 
    {
        bool found = false;

        for (const auto& layerProps : availableLayers) 
        {
            if (strcmp(layerName, layerProps.layerName) == 0) 
            {
                found = true;
                break;
            }
        }

        if (!found)  return false;
    }

    return true;
}

// ------------------------------------------------------------
// Create instance
// ------------------------------------------------------------

void VulkanInstance::create() 
{

    if (enableValidationLayers && !checkValidationLayers()) 
    {
        throw std::runtime_error("Validation layers requested but not available");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan GPU Pipeline";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "RendererPipeline";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    auto extensions = getExtensions();

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (enableValidationLayers) 
    {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    } 
    else 
    {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) 
    {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    std::cout << "[Vulkan] Instance created successfully\n";
}

// ------------------------------------------------------------
// Destroy instance
// ------------------------------------------------------------

void VulkanInstance::destroy() 
{

    if (instance != VK_NULL_HANDLE) 
    {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

