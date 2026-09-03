#pragma once
#include <vulkan/vulkan.h>
#include <string>

class VulkanComputePipeline
{
public:
    // shaderPath defaults to the original fine-culling shader so the
    // pre-existing call site keeps compiling unchanged - a second
    // instance points this at the coarse cluster-culling shader instead
    // (see "Hierarchical / multi-pass GPU culling" in architecture.md).
    void create(
        VkDevice device,
        VkDescriptorSetLayout descriptorLayout,
        const std::string& shaderPath = "shaders/compiled/culling.comp.spv"
    );
    void destroy(VkDevice device);

    VkPipeline       get()    const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};

