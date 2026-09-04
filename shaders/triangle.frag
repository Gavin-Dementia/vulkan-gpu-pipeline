#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/pbr.glsl"

void main()
{
    // Master texture toggle (§44) - material.metallicRoughness.z is a
    // push constant, identical for every fragment in a draw call, so
    // branching around the texture() calls below is uniform control flow
    // (safe for implicit-LOD sampling, no derivative undefined-behavior
    // concern). Off (0.0, the default) reproduces Phase 8 milestone 1's
    // flat, push-constant-only PBR shading - real lighting/shadows/IBL,
    // no material texture detail.
    bool useTextures = material.metallicRoughness.z > 0.5;

    vec3 texColor    = useTextures ? texture(texSampler, fragUV).rgb : vec3(1.0);
    vec3 finalAlbedo = material.albedo.rgb * texColor;

    // glTF channel convention: G = roughness, B = metallic. The push
    // constant stays a *factor* multiplying the texture (same pattern as
    // finalAlbedo above), so grid vs. projectile keep looking visually
    // distinct even sampling the same shared Material (see TECHNICAL_NOTES).
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

    // IBL Milestones 2-3 (see docs/TECHNICAL_NOTES.md §34/§35): full
    // split-sum image-based ambient (diffuse + specular), replacing the
    // old flat 0.03*albedo*ao term. Ambient-specific Fresnel uses NdotV,
    // not dot(H,V) - H is undefined here, there's no single incident
    // light direction for ambient the way there is for the direct term
    // above - and is roughness-aware (fresnelSchlickRoughness, not the
    // direct term's plain fresnelSchlick), used for both halves below.
    vec3 kS_ambient = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD_ambient = (vec3(1.0) - kS_ambient) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = kD_ambient * irradiance * finalAlbedo;

    // Specular half (Milestone 3): prefilteredMap's mip chosen by
    // roughness (mip 0 = mirror-sharp, mip 4 = fully rough - trilinear
    // filtering blends fractional levels), combined with the BRDF LUT's
    // precomputed (scale, bias) pair - Karis's split-sum approximation.
    vec3 R = reflect(-V, N);
    const float MAX_REFLECTION_LOD = 4.0; // prefilteredCubemap_'s mipLevels-1 (5-1) - see VulkanContext::initEnvironment()
    vec3 prefilteredColor = textureLod(prefilteredMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefilteredColor * (kS_ambient * envBRDF.x + envBRDF.y);

    // AO occludes the combined ambient sum, not just the diffuse half -
    // same "don't touch the shadowed direct term" precedent calcShadow()
    // already set for the shadow map, generalized: AO is a general
    // occlusion factor for indirect light reaching a point, with no
    // principled reason to darken diffuse ambient in a crevice but leave
    // a full-brightness specular highlight there.
    vec3 ambient = (diffuseIBL + specularIBL) * ao;
    vec3 color = ambient + Lo;

    // Reinhard tonemap only - no pow(color, 1/2.2) gamma step. The swapchain
    // format is VK_FORMAT_B8G8R8A8_SRGB, so the GPU already linear->sRGB
    // encodes on write; an extra pow() here would double-gamma and wash out.
    color = color / (color + vec3(1.0));

    // material.albedo.a is 1.0 for every draw using the opaque pipeline_
    // (VulkanContext::gridAlpha()'s default), so this is a no-op change
    // for existing behavior - only meaningful once transparentPipeline_
    // (blendEnable=true) is bound instead. See docs/TECHNICAL_NOTES.md §43.
    outColor = vec4(color, material.albedo.a);
}
