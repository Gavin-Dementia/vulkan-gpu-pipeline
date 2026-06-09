#include <GLFW/glfw3.h>
#include <iostream>
#include "vulkan/VulkanContext.h"

int main() {

    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(800, 600, "Vulkan", nullptr, nullptr);

    VulkanContext vk;
    vk.init(window);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }

    vk.cleanup();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

