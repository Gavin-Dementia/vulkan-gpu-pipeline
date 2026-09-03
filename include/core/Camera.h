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
    // aspectRatio is a required, live parameter (see
    // docs/TECHNICAL_NOTES.md §36) - the offscreen scene target's
    // resolution can change at runtime (docked Viewport panel resize),
    // so there's no fixed "the" aspect ratio to bake in as a constant
    // anymore; callers derive it fresh each frame from
    // VulkanContext::sceneColorTarget().extent(), the same "recompute,
    // don't cache" discipline this codebase already applies to frustum
    // planes and lightViewProj().
    glm::mat4 getProjectionMatrix(float aspectRatio) const;

    glm::vec3 position() const { return position_; }

    // True while Ctrl is held (cursor visible, mouse-look suspended) -
    // exposed so Application::mainLoop() can keep ImGui's own mouse
    // capture in sync with it (see the ImGuiConfigFlags_NoMouse toggle
    // there and TECHNICAL_NOTES.md for why that's needed).
    bool cursorVisible() const { return cursorVisible_; }

    // Public so callers (e.g. Projectile launch) can aim along the same
    // direction the camera itself uses for movement/view - single source
    // of truth, same rationale as getViewMatrix()/getProjectionMatrix().
    glm::vec3 getForward() const;

    // Public so FrameRenderer can derive the screen-space projection scale
    // for LOD selection (see VulkanContext::lod1ScreenSize()/lod2ScreenSize())
    // from the same vertical FOV getProjectionMatrix() itself uses - single
    // source of truth, same rationale as getViewMatrix()/getProjectionMatrix().
    static constexpr float FOV_DEGREES = 45.0f;

private:
    static constexpr float NEAR_PLANE   = 0.1f;
    static constexpr float FAR_PLANE    = 200.0f;

    glm::vec3 position_ = { 0.0f, 0.0f, 25.0f };
    float yaw_   = -90.0f;   // 朝向-Z（看向原点）
    float pitch_ = 0.0f;

    float moveSpeed_ = 10.0f;

    // Mouse-look state. The window must have GLFW_CURSOR set to
    // GLFW_CURSOR_DISABLED (done once in Application::init()) so the
    // cursor is hidden and glfwGetCursorPos() reports an unbounded
    // virtual position instead of clamping at the screen edge.
    float mouseSensitivity_ = 0.1f;   // degrees per pixel of mouse motion
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    bool  firstMouseSample_ = true;   // suppress the first frame's jump

    // Holding Ctrl reveals the cursor (GLFW_CURSOR_NORMAL) so the ImGui
    // debug windows can actually be clicked/dragged - GLFW's unbounded
    // virtual position in DISABLED mode doesn't correspond to real screen
    // coordinates, so ImGui can't hit-test against it. Mouse-look is
    // suspended while the cursor is shown, matching the initial value
    // Application::init() sets the window to (disabled = look mode).
    bool cursorVisible_ = false;

    glm::vec3 right() const;
};

