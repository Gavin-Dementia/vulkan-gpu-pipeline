#include "vulkan/frame/FrameRenderer.h"
#include "vulkan/VulkanContext.h"

#include <stdexcept>
#include <iostream>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

void FrameRenderer::init(VulkanContext& ctx)
{
    context = &ctx;

    const int MAX_FRAMES = 2;

    frames.resize(MAX_FRAMES);
    imagesInFlight.resize(getImageCount(), VK_NULL_HANDLE);

    createSyncObjects();
    createCommandBuffers();

    std::cout << "[FrameRenderer] initialized\n";
}

void FrameRenderer::drawFrame()
{
    auto& device = context->device();
    auto& swapchain = context->swapchain();
    auto& renderPass = context->renderPass();
    auto& framebuffer = context->framebuffer();

    FrameContext& frame = frames[currentFrame];

    // 1. WAIT CPU FRAME FENCE (IMPORTANT)
    vkWaitForFences(
        device.get(),
        1,
        &frame.inFlightFence,
        VK_TRUE,
        UINT64_MAX
    );

    vkResetFences(
        device.get(),
        1,
        &frame.inFlightFence
    );

    // 2. ACQUIRE IMAGE
    uint32_t imageIndex = 0;

    VkResult result = vkAcquireNextImageKHR(
        device.get(),
        swapchain.getSwapchain(),
        UINT64_MAX,
        frame.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (result != VK_SUCCESS)
        throw std::runtime_error("Failed to acquire swapchain image"); 

    // 3. WAIT FOR THAT IMAGE IF IN FLIGHT
    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    {
        vkWaitForFences(
            device.get(),
            1,
            &imagesInFlight[imageIndex],
            VK_TRUE,
            UINT64_MAX
        );
    }

    imagesInFlight[imageIndex] = frame.inFlightFence;


    if (imageIndex >= context->framebuffer().get().size())//
        throw std::runtime_error("Framebuffer mismatch with swapchain");

    // 4. RESET COMMAND BUFFER (SAFE NOW)
    vkResetCommandBuffer(frame.commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

    // 5. RENDER PASS (CLEAR SCREEN)
    VkClearValue clearColor{};
    clearColor.color = { {0.1f, 0.2f, 0.7f, 1.0f} };

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass.get();
    rp.framebuffer = framebuffer.get()[imageIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain.getExtent();
    rp.clearValueCount = 1;
    rp.pClearValues = &clearColor;
    
    vkCmdBeginRenderPass(frame.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdEndRenderPass(frame.commandBuffer);

    vkEndCommandBuffer(frame.commandBuffer);

    // 6. SUBMIT
    VkPipelineStageFlags waitStage = 
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailableSemaphore;
    submit.pWaitDstStageMask = &waitStage;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commandBuffer;

    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.renderFinishedSemaphore;

    vkQueueSubmit(
        device.getGraphicsQueue(),
        1,
        &submit,
        frame.inFlightFence
    );

    // 7. PRESENT
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.renderFinishedSemaphore;

    VkSwapchainKHR swapchains[] = { swapchain.getSwapchain() };
    present.swapchainCount = 1;
    present.pSwapchains = swapchains;
    present.pImageIndices = &imageIndex;

    vkQueuePresentKHR(
        device.getPresentQueue(),
        &present
    );
    
    // 8. NEXT FRAME
    currentFrame = (currentFrame + 1) % frames.size();
}

void FrameRenderer::cleanup()
{
    VkDevice device = context->device().get();

    for (auto& frame : frames)
    {
        vkDestroyFence(device, frame.inFlightFence, nullptr);
        vkDestroySemaphore(device, frame.imageAvailableSemaphore, nullptr);
        vkDestroySemaphore(device, frame.renderFinishedSemaphore, nullptr);
    }
}

void FrameRenderer::createSyncObjects()
{
    VkDevice device = context->device().get();

    for (auto& frame : frames)
    {
        // ===== Fence =====
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        // IMPORTANT: start signaled so first frame doesn't stall
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlightFence) != VK_SUCCESS)
            throw std::runtime_error("Failed to create fence");

        // ===== Semaphores =====
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        if (vkCreateSemaphore(device, &semInfo, nullptr, &frame.imageAvailableSemaphore) != VK_SUCCESS)
            throw std::runtime_error("Failed imageAvailable semaphore");

        if (vkCreateSemaphore(device, &semInfo, nullptr, &frame.renderFinishedSemaphore) != VK_SUCCESS)
            throw std::runtime_error("Failed renderFinished semaphore");
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

