#pragma once
#include <glm/glm.hpp>
#include <array>

// Gribb-Hartmann
struct FrustumPlanes
{
    std::array<glm::vec4, 6> planes;
    glm::vec4 cameraPos;      // xyz=cam pos, w=0（padding）

    // LOD distance thresholds - runtime-tunable via the "GPU Culling
    // Stats" ImGui window (VulkanContext::lod1Distance()/lod2Distance())
    // instead of culling.comp's former hardcoded LOD1_DIST/LOD2_DIST
    // constants. x = LOD1_DIST, y = LOD2_DIST, zw unused.
    glm::vec4 lodDistances = glm::vec4(12.0f, 20.0f, 0.0f, 0.0f);

    // zeroToOne selects the near-plane formula matching how viewProj was
    // built: false (default) for GLM's default OpenGL-style z_ndc in
    // [-1,1] (e.g. Camera::getProjectionMatrix(), plain glm::perspective),
    // true for Vulkan's native z_ndc in [0,1] (e.g. VulkanContext::
    // lightViewProj(), built with glm::orthoRH_ZO()). The two conventions
    // only disagree on the near plane (far/left/right/top/bottom are
    // identical): near = m[3]+m[2] for [-1,1], near = m[2] alone for
    // [0,1]. Reusing the [-1,1] formula on a [0,1] matrix silently
    // produces a near plane that never culls anything - the same class of
    // bug TECHNICAL_NOTES.md §22 already hit once for the light's ortho
    // matrix itself (there, glm::ortho() vs. glm::orthoRH_ZO()); this is
    // that same convention mismatch one level up, in plane extraction.
    static FrustumPlanes extractFromMatrix(
        const glm::mat4& viewProj,
        const glm::vec3& camPos,
        bool zeroToOne = false)
    {
        FrustumPlanes result;
        glm::mat4 m = glm::transpose(viewProj);

        result.planes[0] = m[3] + m[0];  // Left
        result.planes[1] = m[3] - m[0];  // Right
        result.planes[2] = m[3] + m[1];  // Bottom
        result.planes[3] = m[3] - m[1];  // Top
        result.planes[4] = zeroToOne ? m[2] : (m[3] + m[2]);  // Near
        result.planes[5] = m[3] - m[2];  // Far

        for (auto& p : result.planes)
        {
            float len = glm::length(glm::vec3(p));
            p /= len;
        }

        result.cameraPos = glm::vec4(camPos, 0.0f);
        return result;
    }
};
static_assert(sizeof(FrustumPlanes) == 128,
    "FrustumPlanes must be 128 bytes (6 vec4 planes + cameraPos + lodDistances) to match FrustumData's std140 GLSL layout");


