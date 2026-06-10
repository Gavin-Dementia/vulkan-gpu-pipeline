#include "vulkan/pipeline/VulkanPipeline.h"
#include <vector>
#include <stdexcept>

static std::vector<char> loadShader(const char* path)
{}; // 你已有的话直接用

void VulkanPipeline::create(
    VkDevice device,
    VkExtent2D extent,
    VkRenderPass renderPass)
{
    // -------------------------
    // 1. Shader stages
    // -------------------------
    auto vertShader = loadShader("shaders/triangle.vert.spv");
    auto fragShader = loadShader("shaders/triangle.frag.spv");

    VkShaderModuleCreateInfo vertInfo{};
    vertInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertInfo.codeSize = vertShader.size();
    vertInfo.pCode = reinterpret_cast<const uint32_t*>(vertShader.data());

    VkShaderModule vertModule;
    vkCreateShaderModule(device, &vertInfo, nullptr, &vertModule);

    VkShaderModuleCreateInfo fragInfo = vertInfo;
    fragInfo.codeSize = fragShader.size();
    fragInfo.pCode = reinterpret_cast<const uint32_t*>(fragShader.data());

    VkShaderModule fragModule;
    vkCreateShaderModule(device, &fragInfo, nullptr, &fragModule);

    VkPipelineShaderStageCreateInfo stages[2]{};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // -------------------------
    // 2. Vertex input (none)
    // -------------------------
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    // -------------------------
    // 3. Input assembly
    // -------------------------
    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // -------------------------
    // 4. Viewport
    // -------------------------
    VkViewport viewport{};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = (float)extent.width;
    viewport.height = (float)extent.height;
    viewport.minDepth = 0;
    viewport.maxDepth = 1;

    VkRect2D scissor{{0,0}, extent};

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    // -------------------------
    // 5. Rasterizer
    // -------------------------
    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.lineWidth = 1.0f;

    // -------------------------
    // 6. Multisample
    // -------------------------
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // -------------------------
    // 7. Color blend
    // -------------------------
    VkPipelineColorBlendAttachmentState cbAttach{};
    cbAttach.colorWriteMask = 0xF;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAttach;

    // -------------------------
    // 8. Pipeline layout
    // -------------------------
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);

    // -------------------------
    // 9. Pipeline create
    // -------------------------
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAsm;
    info.pViewportState = &vp;
    info.pRasterizationState = &rast;
    info.pMultisampleState = &ms;
    info.pColorBlendState = &cb;
    info.layout = layout;
    info.renderPass = renderPass;
    info.subpass = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("Failed to create pipeline");

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);
}

