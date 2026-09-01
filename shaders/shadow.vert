#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec4 inInstancePos;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightViewProj;
    mat4 model;   // same per-draw spin/identity rotation triangle.vert applies
} pc;

void main()
{
    vec3 worldPos = (pc.model * vec4(inPosition, 1.0)).xyz + inInstancePos.xyz;
    gl_Position = pc.lightViewProj * vec4(worldPos, 1.0);
}
