#version 450

// Bakes a procedural gradient sky into one face of the environment
// cubemap (see VulkanCubemap, VulkanContext::initEnvironment()). Runs
// once per face (6 draws total) at startup, not per-frame - see
// docs/TECHNICAL_NOTES.md for why a procedural source (rather than a
// loaded HDR file) was chosen for this milestone, and why the sun
// highlight is intentionally >1.0 (this cubemap is
// VK_FORMAT_R16G16B16A16_SFLOAT, HDR-capable). Output is raw HDR, no
// tonemap - skybox.frag tonemaps when it samples this cubemap live.

layout(location = 0) in vec2 fragNDC;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform SkyCapturePushConstants
{
    mat4 invViewProj;
    vec4 cameraPos;      // xyz = capture eye (origin), w unused
    vec4 sunDirAndCos;   // xyz = normalize(-lightDirection_), w = cos(angular half-size)
} pc;

void main()
{
    vec4 worldPos = pc.invViewProj * vec4(fragNDC, 1.0, 1.0);
    worldPos /= worldPos.w;
    vec3 dir = normalize(worldPos.xyz - pc.cameraPos.xyz);

    vec3 horizonColor = vec3(0.62, 0.72, 0.85);
    vec3 zenithColor  = vec3(0.15, 0.35, 0.75);
    vec3 groundColor  = vec3(0.08, 0.08, 0.09);

    float t = clamp(dir.y, -1.0, 1.0);
    vec3 sky = mix(horizonColor, zenithColor, smoothstep(0.0, 0.6, t));
    sky = mix(sky, groundColor, smoothstep(0.0, -0.15, t));

    // Bright sun disk aligned with the scene's actual directional light
    // (pc.sunDirAndCos.xyz = normalize(-lightDirection_), computed on the
    // CPU at bake time) rather than an independent light source that
    // could drift out of sync with it.
    float sunDot = dot(dir, pc.sunDirAndCos.xyz);
    float sunMask = smoothstep(pc.sunDirAndCos.w - 0.0015, pc.sunDirAndCos.w, sunDot);
    vec3 sunColor = vec3(9.0, 8.0, 6.5);

    outColor = vec4(sky + sunMask * sunColor, 1.0);
}
