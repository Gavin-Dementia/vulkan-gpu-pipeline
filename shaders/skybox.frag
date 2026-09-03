#version 450

// Live per-frame skybox draw: samples the baked environment cubemap
// along the camera's view direction, reconstructed the same
// inverse(viewProj) way envCapture.frag reconstructs its per-face
// direction (see docs/TECHNICAL_NOTES.md) - self-consistent by
// construction, no separately-maintained basis vectors to keep in sync.

layout(location = 0) in vec2 fragNDC;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform SkyboxPushConstants
{
    mat4 invViewProj;
    vec4 cameraPos;
} pc;

void main()
{
    vec4 worldPos = pc.invViewProj * vec4(fragNDC, 1.0, 1.0);
    worldPos /= worldPos.w;
    vec3 dir = normalize(worldPos.xyz - pc.cameraPos.xyz);

    vec3 color = texture(environmentMap, dir).rgb;

    // Same Reinhard tonemap as triangle.frag's last line - visual
    // consistency with the lit geometry it sits behind. No manual gamma
    // pow(): the scene color target is VK_FORMAT_B8G8R8A8_SRGB, GPU
    // auto-encodes on write.
    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);
}
