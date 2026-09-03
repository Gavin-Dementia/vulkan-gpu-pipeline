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

// Phase 8 milestone 2 (see docs/TECHNICAL_NOTES.md): normal/metallic-
// roughness/AO maps. metallicRoughnessMap follows glTF's channel
// convention (G = roughness, B = metallic) so any future swap to a real
// authored/downloaded texture set drops in without a repack.
layout(binding = 4) uniform sampler2D normalMap;
layout(binding = 5) uniform sampler2D metallicRoughnessMap;
layout(binding = 6) uniform sampler2D aoMap;

// IBL Milestone 2 (see docs/TECHNICAL_NOTES.md §34): ambient-lighting
// data, shared globally rather than per-material, so it lives in its own
// descriptor set (set 1) instead of growing the material set above (set
// 0, implicit). Diffuse-only for now - specular IBL is Milestone 3.
layout(set = 1, binding = 0) uniform samplerCube irradianceMap;

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

// Derivative-based tangent frame ("normal mapping without precomputed
// tangents" - Schuler's technique) instead of a tangent vertex attribute:
// this project's Vertex/ObjLoader carry no tangent data, and adding one
// would mean recomputing it for every LOD mesh. dFdx/dFdy on the already-
// available world position and UV are enough to reconstruct a per-pixel
// TBN basis with no vertex-format change - the tradeoff is one that only
// matters at UV seams/poles, invisible at this scene's scale.
//
// Guarded fallback: LOD1/LOD2 (and any UV-less mesh - ObjLoader falls
// back to a constant uv=(0,0) when an OBJ has no texcoord_index) have
// zero UV variation across every pixel, so dFdx(uv)/dFdy(uv) are exactly
// (0,0) there - T and B collapse to the zero vector, and inversesqrt(0)
// is +Inf, so 0*Inf produces NaN that poisons every draw using this
// pipeline, not just the UV-less mesh (hit this directly: the default
// camera distance only ever shows LOD1/LOD2, so the whole scene rendered
// solid black before this guard was added). Skip perturbation and return
// the unperturbed normal whenever the UV gradient is degenerate - exactly
// the correct behavior for a mesh with no real UV data to perturb against.
vec3 perturbNormal(vec3 N, vec3 worldPos, vec2 uv, vec3 mapNormal)
{
    vec3 dp1 = dFdx(worldPos);
    vec3 dp2 = dFdy(worldPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float maxLenSq = max(dot(T, T), dot(B, B));
    if (maxLenSq < EPS)
        return N;

    float invmax = inversesqrt(maxLenSq);
    mat3 TBN = mat3(T * invmax, B * invmax, N);

    return normalize(TBN * mapNormal);
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

    // glTF channel convention: G = roughness, B = metallic. The push
    // constant stays a *factor* multiplying the texture (same pattern as
    // finalAlbedo above), so grid vs. projectile keep looking visually
    // distinct even sampling the same shared Material (see TECHNICAL_NOTES).
    vec3 mr = texture(metallicRoughnessMap, fragUV).rgb;
    float metallic  = material.metallicRoughness.x * mr.b;
    float roughness = material.metallicRoughness.y * mr.g;
    float ao        = texture(aoMap, fragUV).r;

    vec3 mapNormal = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
    vec3 N = normalize(perturbNormal(normalize(fragNormal), fragWorldPos, fragUV, mapNormal));
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

    // IBL Milestone 2 (see docs/TECHNICAL_NOTES.md §34): diffuse-only
    // image-based ambient, replacing the old flat 0.03*albedo*ao term.
    // Ambient-specific Fresnel uses NdotV, not dot(H,V) - H is undefined
    // here, there's no single incident light direction for ambient the
    // way there is for the direct term above. Specular IBL (prefiltered
    // environment + BRDF LUT split-sum) is explicitly Milestone 3, not
    // computed here - only the diffuse half of the split-sum exists.
    // AO occludes this term only, same "don't touch the shadowed direct
    // term" precedent calcShadow() already set for the shadow map.
    vec3 kS_ambient = fresnelSchlick(NdotV, F0);
    vec3 kD_ambient = (vec3(1.0) - kS_ambient) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = kD_ambient * irradiance * finalAlbedo;
    vec3 ambient = diffuseIBL * ao;
    vec3 color = ambient + Lo;

    // Reinhard tonemap only - no pow(color, 1/2.2) gamma step. The swapchain
    // format is VK_FORMAT_B8G8R8A8_SRGB, so the GPU already linear->sRGB
    // encodes on write; an extra pow() here would double-gamma and wash out.
    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);
}
