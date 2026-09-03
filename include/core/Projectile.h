#pragma once
#include <glm/glm.hpp>

// A single mouse-fired object: launches from a point in a fixed direction
// at constant speed and expires after a lifetime. No collision/physics
// response yet - a future milestone reads position() each frame to test
// against the instance grid.
class Projectile
{
public:
    void launch(const glm::vec3& origin, const glm::vec3& direction, float speed = kDefaultSpeed);
    void update(float deltaTime);

    bool isActive() const { return active_; }
    glm::vec3 position() const { return position_; }
    // Where this projectile was at the start of the current update() call,
    // i.e. before this frame's movement was applied - lets a caller sweep
    // a segment [previousPosition(), position()] against collision volumes
    // instead of testing only the post-move point (see
    // docs/TECHNICAL_NOTES.md §41). Meaningless before the first update()
    // following launch(), but nothing reads it before then.
    glm::vec3 previousPosition() const { return previousPosition_; }
    void stop() { active_ = false; }   // immediate deactivation, e.g. on grid impact

private:
    static constexpr float kDefaultSpeed = 30.0f;
    static constexpr float kMaxLifetime  = 5.0f;   // seconds

    glm::vec3 position_         = { 0.0f, 0.0f, 0.0f };
    glm::vec3 previousPosition_ = { 0.0f, 0.0f, 0.0f };
    glm::vec3 direction_ = { 0.0f, 0.0f, -1.0f };
    float     speed_     = 0.0f;
    float     elapsed_   = 0.0f;
    bool      active_    = false;
};

