#pragma once
#include <glm/glm.hpp>

// std140 layout: every member is vec4 (16-byte aligned, 16-byte sized),
// so GLM's natural alignment already satisfies std140 with zero padding.
struct SceneData
{
    glm::vec4 lightDirection; // xyz = direction light travels, w unused
    glm::vec4 lightColor;     // rgb = color, a = intensity
    glm::vec4 cameraPos;      // xyz = world-space camera position, w unused
};
static_assert(sizeof(SceneData) == 48, "SceneData must be 48 bytes (3 x vec4) to match std140 GLSL layout");

// Push constant blocks follow Vulkan's extended alignment rules (effectively
// std430); since every member here is already vec4, std140 and std430 agree
// for this struct, so this layout matches the GLSL push_constant block exactly.
struct MaterialPushConstants
{
    glm::vec4 albedo;            // rgb used, a unused
    glm::vec4 metallicRoughness; // x = metallic, y = roughness, zw unused
};
static_assert(sizeof(MaterialPushConstants) == 32, "MaterialPushConstants must be 32 bytes (2 x vec4)");
