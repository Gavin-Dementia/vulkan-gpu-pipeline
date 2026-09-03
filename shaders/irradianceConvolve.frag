#version 450

// IBL Milestone 2: convolves environmentMap into cosine-weighted diffuse
// irradiance, one texel of irradianceCubemap_ at a time (see
// VulkanContext::initEnvironment(), docs/TECHNICAL_NOTES.md §34). Runs
// once per face (6 draws) at startup, immediately after the environment
// bake, in the same one-shot command buffer. Direction reconstruction
// reuses the exact inverse(viewProj) technique envCapture.frag/
// skybox.frag already established (see §33) - N here is the reconstructed
// per-texel direction of *this* irradiance cubemap face, not a sampled ray.
//
// Reference derivation: the standard cosine-weighted hemisphere Riemann-
// sum diffuse-irradiance integral (LearnOpenGL's IBL diffuse irradiance
// article) - a proven, widely-shipped formula, not hand-derived.

const float PI = 3.14159265359;

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
    vec3 N = normalize(worldPos.xyz - pc.cameraPos.xyz);

    // Arbitrary tangent basis around N - any consistent perpendicular
    // pair works, the double integral below sums to the same result
    // regardless of the basis's rotation about N.
    vec3 up    = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up         = cross(N, right);

    vec3 irradiance = vec3(0.0);
    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // Spherical to tangent-space Cartesian, then to world space.
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            // cos(theta): Lambertian cosine weighting. sin(theta): the
            // solid-angle Jacobian for spherical coordinates - without
            // it, samples near the pole (theta=0) would be
            // over-weighted relative to samples near the horizon.
            irradiance += texture(environmentMap, sampleVec).rgb * cos(theta) * sin(theta);
            nrSamples += 1.0;
        }
    }

    irradiance = PI * irradiance / nrSamples;
    outColor = vec4(irradiance, 1.0);
}
