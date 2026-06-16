#include "vulkan/buffer/VertexBuffer.h"
#include "vulkan/pipeline/VulkanPipeline.h"
#include "vulkan/resource/ShaderLoader.h"
#include "vulkan/device/VulkanDevice.h"
#include "vulkan/instance/InstanceData.h"
#include <vector>
#include <stdexcept>

void VulkanPipeline::create(
    VkDevice device,
    VkExtent2D extent,
    VkRenderPass renderPass,
    VkDescriptorSetLayout descriptorLayout)
{
    // =========================================================
    // 1. Shader modules
    // =========================================================
    VkShaderModule vertModule =
        ShaderLoader::load(device, "shaders/compiled/triangle.vert.spv");

    VkShaderModule fragModule =
        ShaderLoader::load(device, "shaders/compiled/triangle.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};

    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    // =========================================================
    // 2. Vertex input (vertex buffer)
    // =========================================================
    auto vertexBinding = Vertex::getBindingDescription();        // binding=0
    auto vertexAttrs   = Vertex::getAttributeDescriptions();     // location 0,1

    VkVertexInputBindingDescription instanceBinding{};
    instanceBinding.binding   = 1;
    instanceBinding.stride    = sizeof(InstanceData);
    instanceBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;   
    // +：IS per-instance NOT per-vertex

    VkVertexInputAttributeDescription instanceAttr{};
    instanceAttr.binding  = 1;
    instanceAttr.location = 2;   // after Vertex's location 0,1
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
    inputAsm.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAsm.primitiveRestartEnable = VK_FALSE;

    // =========================================================
    // 4. Viewport / Scissor
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
    // 7. Color blend
    // =========================================================
    VkPipelineColorBlendAttachmentState cbAttach{};
    cbAttach.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;

    cbAttach.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.logicOpEnable = VK_FALSE;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAttach;

    // =========================================================
    // 8. Pipeline layout
    // =========================================================
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;                   
    layoutInfo.pSetLayouts    = &descriptorLayout;   

    if (vkCreatePipelineLayout(
            device,
            &layoutInfo,
            nullptr,
            &layout) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create pipeline layout");
    }

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable       = VK_TRUE;
    depthStencil.depthWriteEnable      = VK_TRUE;
    depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    // =========================================================
    // 9. Graphics pipeline
    // =========================================================
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

    info.stageCount = 2;
    info.pStages = stages;

    info.pVertexInputState   = &vertexInputInfo;
    info.pInputAssemblyState = &inputAsm;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rast;
    info.pMultisampleState   = &ms;
    info.pColorBlendState    = &cb;
    info.pDepthStencilState = &depthStencil;

    info.layout     = layout;
    info.renderPass = renderPass;
    info.subpass    = 0;

    if (vkCreateGraphicsPipelines(
            device,
            VK_NULL_HANDLE,
            1,
            &info,
            nullptr,
            &pipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    // =========================================================
    // 10. Cleanup shader modules
    // =========================================================
    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);
}

void VulkanPipeline::destroy(VkDevice device)
{
    if (pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }

    if (layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
}

