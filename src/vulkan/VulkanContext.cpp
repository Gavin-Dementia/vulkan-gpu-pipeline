#include "vulkan/VulkanContext.h"
#include <iostream>

void VulkanContext::init(GLFWwindow* window)
{
    window_ = window;

    instance_.create();
    surface_.create(instance_.get(), window_);

    device_.create(instance_.get(), surface_.get());

    swapchain_.create(
        device_.getPhysical(),
        device_.get(),
        surface_.get(),
        window_
    );

    commandPool_.create(
        device_.get(),
        device_.getGraphicsQueueFamily()
    );

    renderPass_.create(
        device_.get(),
        swapchain_.getImageFormat()
    );

    pipeline_.create(
        device_.get(),
        swapchain_.getExtent(),
        renderPass_.get()
    );

    framebuffer_.create(
        device_.get(),
        renderPass_.get(),
        swapchain_.getImageViews(),
        swapchain_.getExtent()
    );

    std::cout << "Vulkan Context initialized\n";
}

void VulkanContext::cleanup()
{
    pipeline_.destroy(device_.get());
    framebuffer_.destroy(device_.get());
    renderPass_.destroy(device_.get());

    swapchain_.destroy(device_.get());
    commandPool_.destroy(device_.get());

    device_.destroy();
    surface_.destroy(instance_.get());
    instance_.destroy();

    std::cout << "VulkanContext destroyed\n";
}

