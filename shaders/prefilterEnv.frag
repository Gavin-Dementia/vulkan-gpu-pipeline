#version 450

// IBL Milestone 3 (see docs/TECHNICAL_NOTES.md §35): prefilters
// environmentMap into a GGX-importance-sampled specular radiance map,
// one (mip, face) of prefilteredCubemap_ per draw (5 mips x 6 faces = 30
// draws total, see VulkanContext::initEnvironment()). Each mip level
// stores the environment convolved for the roughness that mip
// represents (mip/(mipLevels-1)) - mip 0 = roughness 0 (mirror-sharp),
// higher mips = progressively rougher/blurrier. Direction reconstruction
// reuses the inverse(viewProj) technique envCapture.frag/skybox.frag/
// irradianceConvolve.frag already established.
//
// Reference derivation: Karis/Epic's split-sum GGX importance-sampling
// prefilter (2013) - the standard real-time technique, not hand-derived.
// N=V=R is the standard simplifying assumption this technique uses: the
// full physically-correct prefilter would need a 4D function (direction
// x roughness x view angle), infeasible to precompute into one cubemap;
// assuming the view direction equals the reflection vector collapses it
// to direction x roughness only, which a mip-chain cubemap can store.
// The visible cost is at grazing angles on rough surfaces, where the
// true reflected lobe is stretched relative to R - the same tradeoff
// every real-time renderer shipping this technique accepts.

const float PI = 3.14159265359;

layout(location = 0) in vec2 fragNDC;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform PrefilterPushConstants
{
    mat4 invViewProj;
    vec4 cameraPos;
    float roughness;
} pc;

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

void main()
{
    vec4 worldPos = pc.invViewProj * vec4(fragNDC, 1.0, 1.0);
    worldPos /= worldPos.w;
    vec3 N = normalize(worldPos.xyz - pc.cameraPos.xyz);
    vec3 R = N;
    vec3 V = R;

    float roughness = pc.roughness;

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3 prefilteredColor = vec3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; i++)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            prefilteredColor += texture(environmentMap, L).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / max(totalWeight, 0.0001);
    outColor = vec4(prefilteredColor, 1.0);
}
