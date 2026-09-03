#include "core/Application.h"

#include "vulkan/VulkanContext.h"
#include "vulkan/frame/FrameRenderer.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <algorithm>
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

        // Interactive deltaTime clamp (see docs/TECHNICAL_NOTES.md
        // §37) - off by default, so this project's original
        // uncapped-deltaTime behavior is unchanged unless explicitly
        // toggled on via the "GPU Culling Stats" ImGui window. Applied
        // here, once, upstream of every deltaTime consumer below
        // (Camera::processInput, Projectile::update,
        // updateInstanceSimulation, updateSpin) - the single
        // authoritative clamp point, not re-clamped by each consumer.
        if (context->clampDeltaTimeEnabled())
            deltaTime = std::min(deltaTime, context->maxDeltaTime());

        context->camera().processInput(context->window(), deltaTime);

        // Keep ImGui's own mouse capture in sync with cursor visibility.
        // GLFW_CURSOR_DISABLED reports an unbounded "virtual" position
        // (see Camera::processInput's comments) that ImGui's backend
        // still feeds into io.MousePos - imgui_impl_glfw.cpp explicitly
        // does NOT ignore mouse data on GLFW_CURSOR_DISABLED and
        // recommends ImGuiConfigFlags_NoMouse instead. Without this, that
        // stale/frozen position can land inside a docked ImGui window
        // (virtually the whole client area since Phase 11's dockable
        // viewport) right when Ctrl is released, leaving WantCaptureMouse
        // stuck true and blocking the projectile's click-to-fire trigger
        // below until enough mouse-look movement drifts the position back
        // out of every window's rect - see TECHNICAL_NOTES.md for the
        // full writeup of this bug.
        ImGuiIO& io = ImGui::GetIO();
        if (context->camera().cursorVisible())
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;

        // Poll (not callback) for the left mouse button - ImGui already
        // owns GLFW's mouse callbacks internally (install_callbacks=true
        // in ImGuiLayer::init), so registering our own would clobber it.
        bool leftPressed = glfwGetMouseButton(context->window(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        // Hold Ctrl to reveal the cursor and adjust the ImGui debug windows;
        // release it to go back to mouse-look. Only toggle GLFW's cursor mode
        // on an actual transition, not every frame.
        bool ctrlHeld = glfwGetKey(context->window(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                        glfwGetKey(context->window(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        if (leftPressed && !prevLeftMousePressed_ && !ImGui::GetIO().WantCaptureMouse)
        {
            if(!ctrlHeld)
            {
                glm::vec3 origin = context->camera().position()
                                + context->camera().getForward() * 1.5f;
                context->projectile().launch(origin, context->camera().getForward());
            }// Ctrl held: cursor is visible, don't fire projectiles

        }
        prevLeftMousePressed_ = leftPressed;

        // R resets the grid back to its rest formation - edge-detected
        // the same way as the click trigger above.
        bool rPressed = glfwGetKey(context->window(), GLFW_KEY_R) == GLFW_PRESS;
        if (rPressed && !prevRPressed_ && !ImGui::GetIO().WantCaptureKeyboard)
            context->resetInstanceFormation();
        prevRPressed_ = rPressed;

        // T pauses/resumes the grid's shared spin.
        bool tPressed = glfwGetKey(context->window(), GLFW_KEY_T) == GLFW_PRESS;
        if (tPressed && !prevTPressed_ && !ImGui::GetIO().WantCaptureKeyboard)
            context->toggleSpinPaused();
        prevTPressed_ = tPressed;

        context->projectile().update(deltaTime);
        context->updateInstanceSimulation(deltaTime);
        context->updateSpin(deltaTime);

        if (glfwWindowShouldClose(context->window()) ||
            glfwGetKey(context->window(), GLFW_KEY_ESCAPE) == GLFW_PRESS)
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

