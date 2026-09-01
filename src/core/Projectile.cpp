#include "core/Projectile.h"

void Projectile::launch(const glm::vec3& origin, const glm::vec3& direction, float speed)
{
    position_  = origin;
    direction_ = glm::normalize(direction);
    speed_     = speed;
    elapsed_   = 0.0f;
    active_    = true;
}

void Projectile::update(float deltaTime)
{
    if (!active_) return;

    position_ += direction_ * speed_ * deltaTime;
    elapsed_  += deltaTime;

    if (elapsed_ >= kMaxLifetime)
        active_ = false;
}

