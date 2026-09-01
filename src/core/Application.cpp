#include "core/Application.h"

#include "vulkan/VulkanContext.h"
#include "vulkan/frame/FrameRenderer.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <iostream>
#include "imgui.h"

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

    // Hide + lock the cursor for mouse-look (Camera::processInput reads
    // glfwGetCursorPos every frame). Disabled mode reports an unbounded
    // virtual position instead of clamping at the screen edge. ImGui's
    // GLFW backend explicitly leaves GLFW_CURSOR_DISABLED alone (checked
    // in imgui_impl_glfw.cpp's UpdateMouseCursor), so this doesn't fight
    // the debug overlay.
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    context = new VulkanContext();
    context->init(window);

    frameRenderer = new FrameRenderer();
    frameRenderer->init(*context);

    std::cout << "Application initialized\n";
}

void Application::mainLoop()
{
    std::cout << "Application mainLoop\n";

    float lastTime = (float)glfwGetTime();

    while (running)
    {
        glfwPollEvents();

        float currentTime = (float)glfwGetTime();
        float deltaTime = currentTime - lastTime;
        lastTime = currentTime;

        context->camera().processInput(context->window(), deltaTime);

        // Poll (not callback) for the left mouse button - ImGui already
        // owns GLFW's mouse callbacks internally (install_callbacks=true
        // in ImGuiLayer::init), so registering our own would clobber it.
        bool leftPressed = glfwGetMouseButton(context->window(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (leftPressed && !prevLeftMousePressed_ && !ImGui::GetIO().WantCaptureMouse)
        {
            glm::vec3 origin = context->camera().position()
                              + context->camera().getForward() * 1.5f;
            context->projectile().launch(origin, context->camera().getForward());
        }
        prevLeftMousePressed_ = leftPressed;

        context->projectile().update(deltaTime);

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

    GLFWwindow* window = context->window();

    delete frameRenderer;
    delete context;

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Application cleaned up\n";
}

