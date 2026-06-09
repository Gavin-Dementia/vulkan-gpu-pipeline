#include "core/Application.h"

#include "vulkan/VulkanContext.h"
#include "vulkan/frame/FrameRenderer.h"

#include <GLFW/glfw3.h>
#include <iostream>

void Application::run()
{
    init();
    mainLoop();
    cleanup();
}

void Application::init()
{
    if (!glfwInit())
        throw std::runtime_error("Failed to init GLFW");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(
        1280,
        720,
        "Vulkan Renderer",
        nullptr,
        nullptr
    );

    context = new VulkanContext();
    context->init(window);

    frameRenderer = new FrameRenderer();
    frameRenderer->init(*context);

    std::cout << "Application initialized\n";
}

void Application::mainLoop()
{
    std::cout << "Application mainLoop\n";
    while (running)
    {
        glfwPollEvents();

        if (glfwWindowShouldClose(context->window()))
            running = false;

        frameRenderer->drawFrame();
    }
}

void Application::cleanup()
{
    vkDeviceWaitIdle(context->device().get());

    frameRenderer->cleanup();
    context->cleanup();

    delete frameRenderer;
    delete context;

    GLFWwindow* window = context ? context->window() : nullptr;

    glfwTerminate();

    std::cout << "Application cleaned up\n";
}

