#pragma once
#include <glm/glm.hpp>
#include <array>

// Gribb-Hartmann
struct FrustumPlanes
{
    std::array<glm::vec4, 6> planes;

    static FrustumPlanes extractFromMatrix(const glm::mat4& viewProj)
    {
        FrustumPlanes result;
        glm::mat4 m = glm::transpose(viewProj);

        result.planes[0] = m[3] + m[0];  // Left
        result.planes[1] = m[3] - m[0];  // Right
        result.planes[2] = m[3] + m[1];  // Bottom
        result.planes[3] = m[3] - m[1];  // Top
        result.planes[4] = m[3] + m[2];  // Near
        result.planes[5] = m[3] - m[2];  // Far

        // 归一化每个平面（让法线长度为1，距离才有意义）
        for (auto& p : result.planes)
        {
            float len = glm::length(glm::vec3(p));
            p /= len;
        }

        return result;
    }
};

