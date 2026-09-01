#pragma once
#include <glm/glm.hpp>

// std140 layout: vec4s are 16-byte aligned/sized and mat4 is a natural
// 16-byte-aligned block of 4 vec4 columns, so GLM's layout already
// satisfies std140 with zero padding - no manual alignment needed even
// with the vec4/mat4/vec4 mix below.
struct SceneData
{
    glm::vec4 lightDirection; // xyz = direction light travels, w unused
    glm::vec4 lightColor;     // rgb = color, a = intensity
    glm::vec4 cameraPos;      // xyz = world-space camera position, w unused
    glm::mat4 lightViewProj;  // directional light's orthographic view-proj, for shadow sampling
    glm::vec4 shadowParams;   // x = shadow bias, yzw unused - runtime-tunable via ImGui
};
static_assert(sizeof(SceneData) == 128, "SceneData must be 128 bytes (4 x vec4 + mat4) to match std140 GLSL layout");

// Push constant blocks follow Vulkan's extended alignment rules (effectively
// std430); since every member here is already vec4, std140 and std430 agree
// for this struct, so this layout matches the GLSL push_constant block exactly.
struct MaterialPushConstants
{
    glm::vec4 albedo;            // rgb used, a unused
    glm::vec4 metallicRoughness; // x = metallic, y = roughness, zw unused
};
static_assert(sizeof(MaterialPushConstants) == 32, "MaterialPushConstants must be 32 bytes (2 x vec4)");
