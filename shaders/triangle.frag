#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;

void main()
{
    vec3 texColor = texture(texSampler, fragUV).rgb;
    vec3 normalTint = normalize(fragNormal) * 0.3 + 0.7;
    outColor = vec4(texColor * normalTint, 1.0);
}

