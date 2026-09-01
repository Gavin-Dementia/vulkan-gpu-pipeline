#version 450

const float PI = 3.14159265359;
const float EPS = 1e-4;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec4 fragLightSpacePos;

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;

layout(binding = 2) uniform SceneData {
    vec4 lightDirection; // xyz = direction the light travels; surface-to-light = -xyz
    vec4 lightColor;     // rgb = color, a = intensity
    vec4 cameraPos;      // xyz = world-space camera position
    mat4 lightViewProj;  // unused here - fragLightSpacePos already carries the transformed position
    vec4 shadowParams;   // x = base shadow bias, tunable via the "Lighting" ImGui window
} scene;

layout(binding = 3) uniform sampler2D shadowMap;

layout(push_constant) uniform MaterialPushConstants {
    vec4 albedo;             // rgb used, a unused
    vec4 metallicRoughness;  // x = metallic, y = roughness
} material;

float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness * roughness;
    float a2     = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, EPS);
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denom = NdotV * (1.0 - k) + k;
    return NdotV / max(denom, EPS);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotL, roughness) * geometrySchlickGGX(NdotV, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// lightSpacePos comes from an orthographic (affine) projection, so w is
// always 1 - the divide is kept for generality, not because it matters here.
// VulkanContext::lightViewProj() uses glm::orthoRH_ZO (not glm::ortho),
// so .z is already Vulkan's [0,1] depth range, not OpenGL's [-1,1] -
// only .xy needs the NDC->UV remap.
float calcShadow(vec4 lightSpacePos, vec3 N, vec3 L)
{
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    float currentDepth = proj.z;

    if (currentDepth > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;   // outside the light's frustum - assume lit, not shadowed

    // Slope-scaled bias (steeper-facing surfaces need more) with a flat
    // floor for near-perpendicular ones - avoids shadow acne without
    // introducing visible peter-panning at this scene's scale. Base value
    // is runtime-tunable (scene.shadowParams.x) rather than hardcoded,
    // since curved/detailed meshes (Suzanne's horns/ears) can still show
    // acne right at their silhouette even with a reasonable fixed value.
    float baseBias = scene.shadowParams.x;
    float bias = max(baseBias * (1.0 - dot(N, L)), baseBias * 0.4);

    // 3x3 PCF: averaging multiple depth comparisons turns single-texel
    // acne speckles into a soft, mostly-uniform result instead of a
    // pixel-level noise pattern, and softens shadow edges as a side effect.
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float litSum = 0.0;
    for (int x = -1; x <= 1; x++)
    {
        for (int y = -1; y <= 1; y++)
        {
            float closestDepth = texture(shadowMap, uv + vec2(x, y) * texelSize).r;
            litSum += (currentDepth - bias > closestDepth) ? 0.0 : 1.0;
        }
    }

    return litSum / 9.0;
}

void main()
{
    vec3 texColor    = texture(texSampler, fragUV).rgb;
    vec3 finalAlbedo = material.albedo.rgb * texColor;

    float metallic  = material.metallicRoughness.x;
    float roughness = material.metallicRoughness.y;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(scene.cameraPos.xyz - fragWorldPos);
    vec3 L = normalize(-scene.lightDirection.xyz);
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);

    vec3 F0 = mix(vec3(0.04), finalAlbedo, metallic);
    vec3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

    float NDF = distributionGGX(N, H, roughness);
    float G   = geometrySmith(N, V, L, roughness);

    vec3  numerator   = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + EPS;
    vec3  specular    = numerator / denominator;

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    float shadow = calcShadow(fragLightSpacePos, N, L);

    vec3 radiance = scene.lightColor.rgb * scene.lightColor.a;
    vec3 Lo = (kD * finalAlbedo / PI + specular) * radiance * NdotL * shadow;

    // Flat ambient substitute for missing IBL/environment lighting.
    vec3 ambient = 0.03 * finalAlbedo;
    vec3 color = ambient + Lo;

    // Reinhard tonemap only - no pow(color, 1/2.2) gamma step. The swapchain
    // format is VK_FORMAT_B8G8R8A8_SRGB, so the GPU already linear->sRGB
    // encodes on write; an extra pow() here would double-gamma and wash out.
    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);
}
