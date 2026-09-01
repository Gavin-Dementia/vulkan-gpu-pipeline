#include "core/Camera.h"
#include <glm/gtc/matrix_transform.hpp>

glm::vec3 Camera::getForward() const
{
    glm::vec3 dir;
    dir.x = cos(glm::radians(yaw_)) * cos(glm::radians(pitch_));
    dir.y = sin(glm::radians(pitch_));
    dir.z = sin(glm::radians(yaw_)) * cos(glm::radians(pitch_));
    return glm::normalize(dir);
}

glm::vec3 Camera::right() const
{
    return glm::normalize(glm::cross(getForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

void Camera::processInput(GLFWwindow* window, float deltaTime)
{
    float moveAmount = moveSpeed_ * deltaTime;

    // WASD movement
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        position_ += getForward() * moveAmount;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        position_ -= getForward() * moveAmount;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        position_ -= right() * moveAmount;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        position_ += right() * moveAmount;

    // Hold Ctrl to reveal the cursor and adjust the ImGui debug windows;
    // release it to go back to mouse-look. Only toggle GLFW's cursor mode
    // on an actual transition, not every frame.
    bool ctrlHeld = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;

    if (ctrlHeld != cursorVisible_)
    {
        cursorVisible_ = ctrlHeld;
        glfwSetInputMode(window, GLFW_CURSOR, cursorVisible_ ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
        firstMouseSample_ = true;   // avoid a jump when look mode resumes
    }

    if (cursorVisible_)
        return;   // UI-adjustment mode: skip mouse-look entirely

    // Mouse look (replaces the old QE-up/down + arrow-key yaw/pitch
    // controls). Polled here rather than a GLFW callback, same reasoning
    // as Application's click polling - keeps input handling in one place
    // per frame instead of split across callbacks and polling.
    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);

    if (firstMouseSample_)
    {
        lastMouseX_ = (float)mouseX;
        lastMouseY_ = (float)mouseY;
        firstMouseSample_ = false;
    }

    float dx = (float)mouseX - lastMouseX_;
    float dy = (float)mouseY - lastMouseY_;
    lastMouseX_ = (float)mouseX;
    lastMouseY_ = (float)mouseY;

    yaw_   += dx * mouseSensitivity_;
    pitch_ -= dy * mouseSensitivity_;   // screen Y grows downward; moving the mouse up should look up

    // 限制pitch，避免翻转
    pitch_ = glm::clamp(pitch_, -89.0f, 89.0f);
}

glm::mat4 Camera::getViewMatrix() const
{
    return glm::lookAt(position_, position_ + getForward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::getProjectionMatrix() const
{
    glm::mat4 proj = glm::perspective(
        glm::radians(FOV_DEGREES), ASPECT_RATIO, NEAR_PLANE, FAR_PLANE);
    proj[1][1] *= -1;   // Vulkan's Y axis is flipped relative to OpenGL
    return proj;
}

