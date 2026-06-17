#pragma once
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

class ImGuiLayer
{
public:
    void init(
        GLFWwindow* window,
        VkInstance instance,
        VkPhysicalDevice physical,
        VkDevice device,
        uint32_t queueFamily,
        VkQueue queue,
        VkRenderPass renderPass,
        uint32_t imageCount
    );

    void destroy(VkDevice device);

    void beginFrame();
    void render(VkCommandBuffer cmd);

private:
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

