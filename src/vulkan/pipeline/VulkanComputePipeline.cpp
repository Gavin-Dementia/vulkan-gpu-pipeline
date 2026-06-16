#include "vulkan/pipeline/VulkanComputePipeline.h"
#include "vulkan/resource/ShaderLoader.h"
#include <stdexcept>

void VulkanComputePipeline::create(
    VkDevice device,
    VkDescriptorSetLayout descriptorLayout)
{
    VkShaderModule computeModule =
        ShaderLoader::load(device, "shaders/compiled/culling.comp.spv");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = computeModule;
    stage.pName  = "main";

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &descriptorLayout;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute pipeline layout");

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = stage;
    pipelineInfo.layout = layout_;

    if (vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute pipeline");

    vkDestroyShaderModule(device, computeModule, nullptr);
}

void VulkanComputePipeline::destroy(VkDevice device)
{
    vkDestroyPipeline(device, pipeline_, nullptr);
    vkDestroyPipelineLayout(device, layout_, nullptr);
}

