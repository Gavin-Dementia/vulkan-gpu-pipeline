#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inInstancePos;

layout(location = 0) out vec3 fragNormal;

layout(binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

void main()
{
    fragNormal = inNormal;

    // model matrix still rotating, instance position shifting
    vec3 worldPos = (ubo.model * vec4(inPosition, 1.0)).xyz + inInstancePos.xyz;

    gl_Position = ubo.proj * ubo.view * vec4(worldPos, 1.0);
}

