#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// Matches envCapture.frag's push constant block exactly - 96 bytes, well
// within Vulkan's guaranteed minimum 128-byte push constant size (see
// ShadowPushConstants for the same 128-byte-minimum note elsewhere in
// this codebase).
struct SkyCapturePushConstants
{
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 sunDirAndCos;
};

// One-shot pipeline for baking a procedural environment into a cubemap's
// 6 faces (see VulkanContext::initEnvironment()) - fullscreenTriangle.vert
// + envCapture.frag, no vertex input, no descriptor set, depth disabled
// (this render pass has no depth attachment - see
// VulkanRenderPass::createColorOnly()). Sibling to VulkanShadowPipeline:
// same "new pass gets its own dedicated pipeline class" precedent
// (docs/setup.md §8).
class VulkanEnvCapturePipeline
{
public:
    void create(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass
    );

    void destroy(VkDevice device);

    VkPipeline       get()    const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};
