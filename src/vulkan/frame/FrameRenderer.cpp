#include "vulkan/frame/FrameRenderer.h"
#include "vulkan/VulkanContext.h"

#include <stdexcept>
#include <iostream>
#include <array>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include "vulkan/buffer/UniformBuffer.h"

// Suzanne OBJ world-space center offset → move to origin
// X: (-3.86 + -1.13) / 2 = -2.49
// Y: (0.27 + 2.24) / 2   =  1.25
// Z: (3.25 + 4.96) / 2   =  4.10
static constexpr glm::vec3 SUZANNE_OFFSET = { 2.49f, -1.25f, -4.10f };

void FrameRenderer::init(VulkanContext& ctx)
{
    context = &ctx;

    const int MAX_FRAMES = 2;

    frames.resize(MAX_FRAMES);
    imagesInFlight.resize(getImageCount(), VK_NULL_HANDLE);
    imageRenderFinished.resize(getImageCount());

    createSyncObjects();
    createCommandBuffers();

    // =====================================================
    // FrameGraph
    // =====================================================
    graph = new FrameGraph();
    graph->init(context);

    VkPipeline mainPipeline = context->pipeline().get();

    // =====================================================
    // Pass 0: Geometry
    // =====================================================
    int geometryPass = graph->addPass(RGPass{
        "GeometryPass",
        {},
        {},
        mainPipeline,
        [this](VkCommandBuffer cmd)
        {
            // 每帧更新 MVP 矩阵
            UBOData ubo{};
            ubo.model = glm::rotate(
                glm::translate(glm::mat4(1.0f), SUZANNE_OFFSET),
                (float)glfwGetTime(),          // rotate with time
                glm::vec3(0.0f, 1.0f, 1.0f)
            );
            ubo.view = glm::lookAt(
                glm::vec3(0.0f, 0.0f, 10.0f),  // cam pos
                glm::vec3(0.0f, 0.0f, 0.0f),  // lookat origin
                glm::vec3(0.0f, 1.0f, 0.0f)   // uphup
            );
            ubo.proj = glm::perspective(
                glm::radians(45.0f),
                1280.0f / 720.0f,
                0.1f, 100.0f
            );
            // Vulkan 的 Y 轴和 OpenGL 相反
            ubo.proj[1][1] *= -1;

            context->uniformBuffer().update(context->device().get(), ubo);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipeline().get());

            // bind DescriptorSet（notify where GPU uniform buffer is）
            VkDescriptorSet ds = context->descriptor().set();
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                context->pipeline().getLayout(),   // 需要 pipeline layout
                0, 1, &ds,
                0, nullptr
            );

            context->vertexBuffer().bind(cmd);
            vkCmdDraw(cmd, context->vertexBuffer().vertexCount(), 1, 0, 0);
        }
    });

    // =====================================================
    // Pass 1: Lighting (no pipeline yet → Stage B placeholder)
    // =====================================================
    int lightingPass = graph->addPass(RGPass{
        "LightingPass",
        { geometryPass },
        {},
        VK_NULL_HANDLE,
        [](VkCommandBuffer cmd)
        {
            // Stage B: logic placeholder
            // later -> compute / fullscreen quad
        }
    });

    // =====================================================
    // Pass 2: PostProcess
    // =====================================================
    graph->addPass(RGPass{
        "PostProcess",
        { lightingPass },
        {},
        VK_NULL_HANDLE,
        [](VkCommandBuffer cmd)
        {
            // Stage B: placeholder
        }
    });

    // =====================================================
    // Build DAG
    // =====================================================
    graph->build();

    std::cout << "[FrameRenderer] initialized (FrameGraph Stage B)\n";
}

void FrameRenderer::drawFrame()
{
    auto& device = context->device();
    auto& swapchain = context->swapchain();
    auto& renderPass = context->renderPass();
    auto& framebuffer = context->framebuffer();

    FrameContext& frame = frames[currentFrame];

    vkWaitForFences(device.get(), 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device.get(), 1, &frame.inFlightFence);

    uint32_t imageIndex;
    vkAcquireNextImageKHR(
        device.get(),
        swapchain.getSwapchain(),
        UINT64_MAX,
        frame.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    {
        vkWaitForFences(device.get(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight[imageIndex] = frame.inFlightFence;

    vkResetCommandBuffer(frame.commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

    // VkClearValue clear{};
    // clear.color = {0.1f, 0.2f, 0.7f, 1.0f};
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = { 0.1f, 0.2f, 0.7f, 1.0f };
    clearValues[1].depthStencil = { 1.0f, 0 };   // depth=1.0（最远），stencil=0

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass.get();
    rp.framebuffer = framebuffer.get()[imageIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain.getExtent();
    rp.clearValueCount = static_cast<uint32_t>(clearValues.size());
    rp.pClearValues    = clearValues.data();

    vkCmdBeginRenderPass(frame.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // ===== FrameGraph execution =====
    graph->execute(frame.commandBuffer);

    vkCmdEndRenderPass(frame.commandBuffer);

    vkEndCommandBuffer(frame.commandBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailableSemaphore;
    submit.pWaitDstStageMask = &waitStage;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commandBuffer;

    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &imageRenderFinished[imageIndex];

    // std::cout
    // << "imageIndex = "
    // << imageIndex
    // << ", semaphore = "
    // << imageRenderFinished[imageIndex]
    // << std::endl;

    vkQueueSubmit(device.getGraphicsQueue(), 1, &submit, frame.inFlightFence);

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &imageRenderFinished[imageIndex];

    VkSwapchainKHR swapchains[] = { swapchain.getSwapchain() };
    present.swapchainCount = 1;
    present.pSwapchains = swapchains;
    present.pImageIndices = &imageIndex;

    vkQueuePresentKHR(device.getPresentQueue(), &present);

    currentFrame = (currentFrame + 1) % frames.size();
}

void FrameRenderer::cleanup()
{
    VkDevice device = context->device().get();

    for (auto& frame : frames)
    {
        vkDestroyFence(
            device,
            frame.inFlightFence,
            nullptr);

        vkDestroySemaphore(
            device,
            frame.imageAvailableSemaphore,
            nullptr);
    }

    for (auto semaphore : imageRenderFinished)
    {
        vkDestroySemaphore(
            device,
            semaphore,
            nullptr);
    }

    delete graph;
}

void FrameRenderer::createSyncObjects()
{
    VkDevice device = context->device().get();

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // Per-frame resources
    for (auto& frame : frames)
    {
        if (vkCreateFence(
                device,
                &fenceInfo,
                nullptr,
                &frame.inFlightFence) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create fence");
        }

        if (vkCreateSemaphore(
                device,
                &semInfo,
                nullptr,
                &frame.imageAvailableSemaphore) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Failed imageAvailable semaphore");
        }
    }

    // Per-swapchain-image render finished semaphores
    for (auto& semaphore : imageRenderFinished)
    {
        if (vkCreateSemaphore(
                device,
                &semInfo,
                nullptr,
                &semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Failed imageRenderFinished semaphore");
        }
    }
}

void FrameRenderer::createCommandBuffers()
{
    VkDevice device = context->device().get();
    VkCommandPool pool = context->commandPool().get();

    for (auto& frame : frames)
    {
        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device, &info, &frame.commandBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate command buffer");
        }
    }
}

