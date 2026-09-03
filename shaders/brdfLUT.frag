#version 450

// IBL Milestone 3 (see docs/TECHNICAL_NOTES.md §35): bakes the BRDF
// integration LUT - the second half of Karis's split-sum specular IBL
// approximation, a pure function of (NdotV, roughness) with no
// environment/texture dependency at all (unlike every other bake in
// this codebase, this one needs no descriptor set and no push constant -
// just the UV this fullscreen triangle already provides). One draw,
// one 512x512 target, see VulkanContext::initEnvironment().
//
// IMPORTANT: this shader's geometry term uses k_IBL = roughness^2 / 2,
// NOT triangle.frag's geometrySchlickGGX's direct-light k = (roughness+1)^2/8.
// These are two different, both-correct-in-their-own-context remappings
// from Karis's paper: k_direct is tuned for analytic direct lights,
// k_IBL for the Monte-Carlo-integrated ambient case computed here. Using
// the direct-light k here would silently produce plausible-but-wrong
// (too-dark-at-grazing-angles) LUT values, not a crash - do not "fix"
// this to match triangle.frag's version, they're supposed to differ.

const float PI = 3.14159265359;

layout(location = 0) in vec2 fragNDC;
layout(location = 0) out vec4 outColor;

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
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

// IBL-specific k remapping - see the file-level comment above.
float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
    float k = (roughness * roughness) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith_IBL(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX_IBL(NdotL, roughness) * GeometrySchlickGGX_IBL(NdotV, roughness);
}

vec2 IntegrateBRDF(float NdotV, float roughness)
{
    vec3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    vec3 N = vec3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; i++)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0)
        {
            float G = GeometrySmith_IBL(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);
    return vec2(A, B);
}

void main()
{
    vec2 uv = fragNDC * 0.5 + 0.5;   // NDC [-1,1] -> [0,1]
    vec2 integratedBRDF = IntegrateBRDF(uv.x, uv.y);
    outColor = vec4(integratedBRDF, 0.0, 1.0);
}
