#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inInstancePos;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragWorldPos;

layout(binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

void main()
{
    vec3 worldPos = (ubo.model * vec4(inPosition, 1.0)).xyz + inInstancePos.xyz;

    // ubo.model is a pure rotation (no scale) for every draw using this
    // shader, so mat3(model) correctly rotates normals; a full inverse-
    // transpose normal matrix is only needed with non-uniform scale.
    fragNormal   = mat3(ubo.model) * inNormal;
    fragUV       = inUV;
    fragWorldPos = worldPos;

    gl_Position = ubo.proj * ubo.view * vec4(worldPos, 1.0);
}
