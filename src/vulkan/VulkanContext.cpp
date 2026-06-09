#include <vulkan/VulkanContext.h>
#include <iostream>
#include <cstring>
#include <GLFW/glfw3.h>
#include <stdexcept>

void VulkanContext::init(GLFWwindow* window) {

    // auto support = querySupport(physicalDevice, surface);

    instance.create();
    surface.create(instance.get(), window);
    device.create(instance.get(), surface.get());

    // if (support.formats.empty() || support.presentModes.empty()) {
    //     throw std::runtime_error("Swapchain support incomplete");
    // }

    swapchain.create(
        device.getPhysical(),
        device.get(),
        surface.get(),
        window
    );

    std::cout << "Vulkan Context initialized\n";
}

void VulkanContext::cleanup() {

    device.destroy();

    surface.destroy(instance.get());

    instance.destroy();

    swapchain.destroy(device.get());

    std::cout << "Vulkan Context destroyed\n";
}

