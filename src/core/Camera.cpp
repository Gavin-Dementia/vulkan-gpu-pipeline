#include "core/Camera.h"
#include <glm/gtc/matrix_transform.hpp>

glm::vec3 Camera::forward() const
{
    glm::vec3 dir;
    dir.x = cos(glm::radians(yaw_)) * cos(glm::radians(pitch_));
    dir.y = sin(glm::radians(pitch_));
    dir.z = sin(glm::radians(yaw_)) * cos(glm::radians(pitch_));
    return glm::normalize(dir);
}

glm::vec3 Camera::right() const
{
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

void Camera::processInput(GLFWwindow* window, float deltaTime)
{
    float moveAmount = moveSpeed_ * deltaTime;
    float turnAmount = turnSpeed_ * deltaTime;

    // WASD movement
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        position_ += forward() * moveAmount;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        position_ -= forward() * moveAmount;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        position_ -= right() * moveAmount;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        position_ += right() * moveAmount;

    // QE 上下
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
        position_.y -= moveAmount;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
        position_.y += moveAmount;

    // 方向键转向
    if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
        yaw_ -= turnAmount;
    if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
        yaw_ += turnAmount;
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
        pitch_ += turnAmount;
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
        pitch_ -= turnAmount;

    // 限制pitch，避免翻转
    pitch_ = glm::clamp(pitch_, -89.0f, 89.0f);
}

glm::mat4 Camera::getViewMatrix() const
{
    return glm::lookAt(position_, position_ + forward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::getProjectionMatrix() const
{
    glm::mat4 proj = glm::perspective(
        glm::radians(FOV_DEGREES), ASPECT_RATIO, NEAR_PLANE, FAR_PLANE);
    proj[1][1] *= -1;   // Vulkan's Y axis is flipped relative to OpenGL
    return proj;
}

