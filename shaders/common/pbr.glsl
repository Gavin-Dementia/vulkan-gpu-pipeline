// Shared fragment-stage declarations + Cook-Torrance BRDF / PCF-shadow /
// derivative-tangent-frame helper functions, #include'd by triangle.frag
// and triangle_refractive.frag. Phase 23 M1 forked triangle_refractive.frag
// from triangle.frag verbatim because a shared shader can't statically
// reference the 3rd descriptor set only refractivePipeline_ binds (see
// triangle_refractive.frag's own header comment for why) - that fork left
// ~200 lines needing hand-sync on every future PBR/shadow change. This
// file is that fix: everything through the direct+ambient PBR terms lives
// here once; each .frag's main() (plus, for the refractive variant, its
// own extra set-2 sampler declaration) is the only thing that actually
// differs between them. Requires -I <repo>/shaders on the glslc command
// line (see CMakeLists.txt) so the #include path below resolves, and
// `#extension GL_GOOGLE_include_directive` in the including file (glslc's
// own extension for source-level #include, not core GLSL).

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
    vec4 shadowParams;   // x = base shadow bias; y/z = sceneColorTarget_'s
                          // width/height in pixels (Phase 23 M1, triangle_refractive.frag
                          // only - see SceneData.h); w unused
} scene;

layout(binding = 3) uniform sampler2D shadowMap;

// Phase 8 milestone 2: normal/metallic-roughness/AO maps. metallicRoughnessMap
// follows glTF's channel convention (G = roughness, B = metallic) so any
// future swap to a real authored/downloaded texture set drops in without
// a repack.
layout(binding = 4) uniform sampler2D normalMap;
layout(binding = 5) uniform sampler2D metallicRoughnessMap;
layout(binding = 6) uniform sampler2D aoMap;

// IBL Milestones 2-3: ambient-lighting data, shared globally rather than
// per-material, so it lives in its own descriptor set (set 1) instead of
// growing the material set above (set 0, implicit). irradianceMap =
// diffuse (M2); prefilteredMap + brdfLUT together = specular, Karis's
// split-sum approximation (M3).
layout(set = 1, binding = 0) uniform samplerCube irradianceMap;
layout(set = 1, binding = 1) uniform samplerCube prefilteredMap;
layout(set = 1, binding = 2) uniform sampler2D brdfLUT;

layout(push_constant) uniform MaterialPushConstants {
    vec4 albedo;             // rgb used, a = opacity (transparentPipeline_ only)
    vec4 metallicRoughness;  // x = metallic, y = roughness, z = use
                             // texture maps (1.0) vs. flat push-constant-
                             // only shading (0.0) - see §44; w = index of
                             // refraction, triangle_refractive.frag only - see §45
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

// Roughness-aware Fresnel for ambient/IBL use (Sébastien Lagarde's
// variant, standard in split-sum IBL implementations) - the plain
// fresnelSchlick() above is only meaningful for a single incident
// direction (the direct light term uses dot(H,V)); ambient light arrives
// from every direction, and a rough surface's Fresnel edge brightening is
// less pronounced than a smooth one's, which this clamps for via
// max(vec3(1.0-roughness), F0) instead of a flat vec3(1.0).
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Derivative-based tangent frame ("normal mapping without precomputed
// tangents" - Schuler's technique) instead of a tangent vertex attribute:
// this project's Vertex/ObjLoader carry no tangent data. dFdx/dFdy on the
// already-available world position and UV are enough to reconstruct a
// per-pixel TBN basis with no vertex-format change - the tradeoff is one
// that only matters at UV seams/poles, invisible at this scene's scale.
//
// Guarded fallback: a mesh with no real UV variation has zero-vector
// dFdx(uv)/dFdy(uv), which would otherwise poison this with NaN
// (inversesqrt(0) = +Inf, 0*Inf = NaN) - skip perturbation and return the
// unperturbed normal whenever the UV gradient is degenerate.
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
// VulkanContext::lightViewProj() uses glm::orthoRH_ZO (not glm::ortho()),
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
    // introducing visible peter-panning at this scene's scale.
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
