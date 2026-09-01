#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

class Camera
{
public:
    void processInput(GLFWwindow* window, float deltaTime);

    glm::mat4 getViewMatrix() const;

    // Single source of truth for the projection matrix - shared by the
    // culling compute pass (frustum construction) and the geometry pass
    // (vertex transform), the same way getViewMatrix() already is. Bakes
    // in the Vulkan Y-flip (proj[1][1] *= -1) so callers never repeat it.
    glm::mat4 getProjectionMatrix() const;

    glm::vec3 position() const { return position_; }

private:
    static constexpr float FOV_DEGREES  = 45.0f;
    static constexpr float ASPECT_RATIO = 1280.0f / 720.0f;   // fixed window size, not resizable
    static constexpr float NEAR_PLANE   = 0.1f;
    static constexpr float FAR_PLANE    = 200.0f;

    glm::vec3 position_ = { 0.0f, 0.0f, 25.0f };
    float yaw_   = -90.0f;   // 朝向-Z（看向原点）
    float pitch_ = 0.0f;

    float moveSpeed_ = 10.0f;
    float turnSpeed_ = 60.0f;   // 度/秒

    glm::vec3 forward() const;
    glm::vec3 right() const;
};

