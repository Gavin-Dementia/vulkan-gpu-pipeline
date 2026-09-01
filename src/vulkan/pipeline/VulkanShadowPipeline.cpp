#include "vulkan/pipeline/VulkanShadowPipeline.h"
#include "vulkan/resource/ShaderLoader.h"
#include "vulkan/buffer/VertexBuffer.h"
#include "vulkan/instance/InstanceData.h"
#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <stdexcept>

void VulkanShadowPipeline::create(
    VkDevice device,
    VkExtent2D extent,
    VkRenderPass renderPass)
{
    // =========================================================
    // 1. Shader modules - vertex only, no fragment stage
    // =========================================================
    VkShaderModule vertModule =
        ShaderLoader::load(device, "shaders/compiled/shadow.vert.spv");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertModule;
    stage.pName  = "main";

    // =========================================================
    // 2. Vertex input - same layout as the main pipeline (VulkanPipeline
    //    ::create): binding 0 = Vertex, binding 1 = per-instance position.
    //    shadow.vert only consumes locations 0 and 3; a pipeline may
    //    supply attributes a shader doesn't read.
    // =========================================================
    auto vertexBinding = Vertex::getBindingDescription();
    auto vertexAttrs   = Vertex::getAttributeDescriptions();

    VkVertexInputBindingDescription instanceBinding{};
    instanceBinding.binding   = 1;
    instanceBinding.stride    = sizeof(InstanceData);
    instanceBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription instanceAttr{};
    instanceAttr.binding  = 1;
    instanceAttr.location = 3;
    instanceAttr.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    instanceAttr.offset   = 0;

    std::array<VkVertexInputBindingDescription, 2> bindings = { vertexBinding, instanceBinding };

    std::vector<VkVertexInputAttributeDescription> attrs(vertexAttrs.begin(), vertexAttrs.end());
    attrs.push_back(instanceAttr);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount   = static_cast<uint32_t>(bindings.size());
    vertexInputInfo.pVertexBindingDescriptions      = bindings.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInputInfo.pVertexAttributeDescriptions    = attrs.data();

    // =========================================================
    // 3. Input assembly
    // =========================================================
    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAsm.primitiveRestartEnable = VK_FALSE;

    // =========================================================
    // 4. Viewport / Scissor - sized to the shadow map, not the swapchain
    // =========================================================
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width  = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    // =========================================================
    // 5. Rasterizer
    // =========================================================
    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.depthClampEnable        = VK_FALSE;
    rast.rasterizerDiscardEnable = VK_FALSE;
    rast.polygonMode             = VK_POLYGON_MODE_FILL;
    rast.lineWidth               = 1.0f;
    rast.cullMode                = VK_CULL_MODE_NONE;
    rast.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.depthBiasEnable         = VK_FALSE;

    // =========================================================
    // 6. Multisampling
    // =========================================================
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.sampleShadingEnable = VK_FALSE;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // =========================================================
    // 7. Color blend - no color attachments in this subpass, but still
    //    pass a valid (empty) struct rather than nullptr
    // =========================================================
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.logicOpEnable = VK_FALSE;
    cb.attachmentCount = 0;
    cb.pAttachments = nullptr;

    // =========================================================
    // 8. Depth/stencil
    // =========================================================
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable       = VK_TRUE;
    depthStencil.depthWriteEnable      = VK_TRUE;
    depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    // =========================================================
    // 9. Pipeline layout - no descriptor sets, one push constant
    // =========================================================
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 0;
    layoutInfo.pSetLayouts            = nullptr;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow pipeline layout");

    // =========================================================
    // 10. Graphics pipeline
    // =========================================================
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 1;
    info.pStages = &stage;
    info.pVertexInputState   = &vertexInputInfo;
    info.pInputAssemblyState = &inputAsm;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rast;
    info.pMultisampleState   = &ms;
    info.pColorBlendState    = &cb;
    info.pDepthStencilState  = &depthStencil;
    info.layout     = layout_;
    info.renderPass = renderPass;
    info.subpass    = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow pipeline");

    vkDestroyShaderModule(device, vertModule, nullptr);
}

void VulkanShadowPipeline::destroy(VkDevice device)
{
    if (pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }

    if (layout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}
