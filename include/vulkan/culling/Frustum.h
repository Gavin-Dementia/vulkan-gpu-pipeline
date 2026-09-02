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

    static FrustumPlanes extractFromMatrix(
        const glm::mat4& viewProj,
        const glm::vec3& camPos)
    {
        FrustumPlanes result;
        glm::mat4 m = glm::transpose(viewProj);

        result.planes[0] = m[3] + m[0];  // Left
        result.planes[1] = m[3] - m[0];  // Right
        result.planes[2] = m[3] + m[1];  // Bottom
        result.planes[3] = m[3] - m[1];  // Top
        result.planes[4] = m[3] + m[2];  // Near
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


