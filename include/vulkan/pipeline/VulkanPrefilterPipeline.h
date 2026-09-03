#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// Matches prefilterEnv.frag's push constant block exactly. Diverges from
// SkyboxPushConstants (invViewProj + cameraPos only) because this shader
// needs an extra field (roughness) - the established "diverge only when
// data needs differ" rule (see VulkanIrradiancePipeline's header comment
// for the counter-example where reuse was the right call).
struct PrefilterPushConstants
{
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    float     roughness;
};

// One-shot pipeline for baking one mip level of prefilteredCubemap_'s
// specular prefilter (see VulkanContext::initEnvironment()) -
// fullscreenTriangle.vert + prefilterEnv.frag, 1 descriptor set (reuses
// CubeSamplerDescriptor's layout - same "sample environmentCubemap_"
// shape the irradiance bake's input already uses). One instance per mip
// level (5 total, each viewport-sized to that mip's face size) - every
// pipeline class in this codebase bakes a static viewport at creation
// time, no dynamic-viewport-state precedent exists to reuse instead.
// Depth disabled (no depth attachment in its render pass). Sibling to
// VulkanEnvCapturePipeline/VulkanSkyboxPipeline/VulkanIrradiancePipeline.
class VulkanPrefilterPipeline
{
public:
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass,
        VkDescriptorSetLayout descriptorLayout
    );

    void destroy(VkDevice device);

    VkPipeline       get()    const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};
