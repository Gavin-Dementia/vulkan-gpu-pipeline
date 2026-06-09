#pragma once

#include <memory>

class VulkanContext;
class FrameRenderer;

class Application
{
public:
    void run();

private:
    void init();
    void mainLoop();
    void cleanup();

private:
    VulkanContext* context = nullptr;
    FrameRenderer* frameRenderer = nullptr;

    bool running = true;
};

