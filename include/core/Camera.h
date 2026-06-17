#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

class Camera
{
public:
    void processInput(GLFWwindow* window, float deltaTime);

    glm::mat4 getViewMatrix() const;
    glm::vec3 position() const { return position_; }

private:
    glm::vec3 position_ = { 0.0f, 0.0f, 25.0f };
    float yaw_   = -90.0f;   // 朝向-Z（看向原点）
    float pitch_ = 0.0f;

    float moveSpeed_ = 10.0f;
    float turnSpeed_ = 60.0f;   // 度/秒

    glm::vec3 forward() const;
    glm::vec3 right() const;
};

