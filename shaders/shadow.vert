#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec4 inInstancePos;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightViewProj;
} pc;

void main()
{
    vec3 worldPos = inPosition + inInstancePos.xyz;
    gl_Position = pc.lightViewProj * vec4(worldPos, 1.0);
}
