#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 23 M1 (docs/roadmap.md / docs/TECHNICAL_NOTES.md §45) - refractive
// glass/jelly/liquid variant of triangle.frag. A separate file rather than
// a branch inside triangle.frag: this shader statically references a 3rd
// descriptor set (sceneColorCopy below) that pipeline_/transparentPipeline_
// never bind, and a pipeline's shader modules must be compatible with its
// own VkPipelineLayout - branching inside one shared triangle.frag would
// force that 3rd set onto every pipeline using it. Everything through the
// direct+ambient PBR terms is shared with triangle.frag via
// common/pbr.glsl (closing the hand-sync risk that used to come with this
// fork - see that file's header comment); only the extra set-2 sampler
// below and the final composite in main() are unique to this file.
#include "common/pbr.glsl"

// The previous frame's fully-composited scene (VulkanContext::
// sceneColorCopy_) - vkCmdCopyImage'd from sceneColorTarget_ once per
// frame in FrameRenderer, before this frame's own scene render pass
// begins (see that call site for why this has to be the *previous*
// frame's image: Vulkan can't sample and write the same attachment in one
// pass, and this project's GeometryPass is deliberately not split into two
// render pass instances to get a same-frame capture - see the roadmap
// entry for the risk/complexity tradeoff). One frame of temporal lag,
// imperceptible at normal camera speeds - same "known, accepted
// approximation" bar as the screen-space LOD math (§14) and the no-
// debounce resize stalls (§36/§39).
layout(set = 2, binding = 0) uniform sampler2D sceneColorCopy;

void main()
{
    bool useTextures = material.metallicRoughness.z > 0.5;

    vec3 texColor    = useTextures ? texture(texSampler, fragUV).rgb : vec3(1.0);
    vec3 finalAlbedo = material.albedo.rgb * texColor;

    vec3 mr = useTextures ? texture(metallicRoughnessMap, fragUV).rgb : vec3(1.0);
    float metallic  = material.metallicRoughness.x * mr.b;
    float roughness = material.metallicRoughness.y * mr.g;
    float ao        = useTextures ? texture(aoMap, fragUV).r : 1.0;

    vec3 N;
    if (useTextures)
    {
        vec3 mapNormal = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
        N = normalize(perturbNormal(normalize(fragNormal), fragWorldPos, fragUV, mapNormal));
    }
    else
    {
        N = normalize(fragNormal);
    }
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

    vec3 kS_ambient = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD_ambient = (vec3(1.0) - kS_ambient) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = kD_ambient * irradiance * finalAlbedo;

    vec3 R = reflect(-V, N);
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefilteredColor = textureLod(prefilteredMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefilteredColor * (kS_ambient * envBRDF.x + envBRDF.y);

    vec3 ambient = (diffuseIBL + specularIBL) * ao;
    vec3 surfaceResponse = ambient + Lo;   // reflections/highlights off the glass surface itself

    // ---- Refraction: sample the previous frame's scene through a bent
    // ray instead of straight through. ior comes from the push constant
    // (default 1.5, glass-like); refract()'s eta is 1/ior since V points
    // from the surface toward the camera (air) into the material.
    float ior = max(material.metallicRoughness.w, 1.0);
    vec3 refractDir = refract(-V, N, 1.0 / ior);

    // No view/projection matrix is available in this fragment stage (only
    // world-space vectors), so the refracted ray's screen-space pixel
    // offset is approximated rather than reprojected exactly: build a
    // local screen-tangent basis from the straight-through view ray (-V)
    // and measure how far refractDir deviates from it along that basis.
    // Bigger IOR -> bigger deviation -> bigger visible bend. Same
    // small-angle-approximation spirit as the screen-space LOD math (§14),
    // not exact reprojection.
    vec3 worldUp     = (abs(V.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 screenRight = normalize(cross(worldUp, V));
    vec3 screenUp    = cross(V, screenRight);
    vec3 deviation   = refractDir - (-V);
    vec2 pixelOffset = vec2(dot(deviation, screenRight), -dot(deviation, screenUp));

    const float kRefractionStrength = 0.35;  // hand-tuned, not exposed (see roadmap's M1 scope note)
    vec2 screenUV  = gl_FragCoord.xy / max(scene.shadowParams.yz, vec2(1.0));
    vec2 refractUV = screenUV + pixelOffset * kRefractionStrength;

    vec3 transmitted = texture(sceneColorCopy, refractUV).rgb * finalAlbedo;

    // Fresnel-weighted mix: near-grazing angles (high F) look more like a
    // mirror-ish surface (surfaceResponse), near-direct angles (low F) look
    // more like seeing through the material (transmitted).
    float fresnelMix = clamp(dot(F, vec3(1.0 / 3.0)), 0.0, 1.0);
    vec3 color = mix(transmitted, surfaceResponse, fresnelMix);

    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);   // refractive draws don't use gridAlpha()/blending - see roadmap
}
