#version 450

const float PI = 3.14159265359;
const float EPS = 1e-4;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;

layout(binding = 2) uniform SceneData {
    vec4 lightDirection; // xyz = direction the light travels; surface-to-light = -xyz
    vec4 lightColor;     // rgb = color, a = intensity
    vec4 cameraPos;      // xyz = world-space camera position
} scene;

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

    vec3 radiance = scene.lightColor.rgb * scene.lightColor.a;
    vec3 Lo = (kD * finalAlbedo / PI + specular) * radiance * NdotL;

    // Flat ambient substitute for missing IBL/environment lighting.
    vec3 ambient = 0.03 * finalAlbedo;
    vec3 color = ambient + Lo;

    // Reinhard tonemap only - no pow(color, 1/2.2) gamma step. The swapchain
    // format is VK_FORMAT_B8G8R8A8_SRGB, so the GPU already linear->sRGB
    // encodes on write; an extra pow() here would double-gamma and wash out.
    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);
}
