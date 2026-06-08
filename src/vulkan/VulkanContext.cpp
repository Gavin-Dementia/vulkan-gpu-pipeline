#include "vulkan/VulkanContext.h"
#include <vulkan/vulkan.h>
#include <iostream>
#include <cstring>
#include <GLFW/glfw3.h>
#include <stdexcept>

void VulkanContext::init(GLFWwindow* window) {
    instance.create();
    surface.create(instance.get(), window);
    device.create(instance.get(), surface.get());

    swapchain.create(
        device.getPhysical(),
        device.get(),
        surface.get(),
        window
    );

    imageView.create(
        device.get(),
        swapchain.getImages(),
        swapchain.getFormat()
    );

    renderPass.create(device.get(), swapchain.getFormat());

    framebuffer.create(
        device.get(),
        renderPass.get(),
        imageView.get(),
        swapchain.getExtent()
    );

    commandPool.create(device.get(), device.getGraphicsQueueFamilyIndex());

    createCommandBuffer();

    setupSyncObjects(device.get());

    sync.create(
        device.get(),
        2, // MAX_FRAMES_IN_FLIGHT
        swapchain.getImages().size()
    );

    std::cout << "Vulkan Context initialized\n";
}

void VulkanContext::cleanup() {

    device.destroy();

    surface.destroy(instance.get());

    instance.destroy();

    swapchain.destroy(device.get());

    imageView.destroy(device.get());

    renderPass.destroy(device.get());

    framebuffer.destroy(device.get());

    std::cout << "Vulkan Context destroyed\n";
}

void VulkanContext::createCommandBuffer() {

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool.get();
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device.get(), &alloc, &commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffer");
    }

    std::cout << "[Vulkan] CommandBuffer allocated\n";
}

void VulkanContext::drawFrame()
{
    FrameSync& frame = sync.getCurrentFrame();

    // 1. CPU wait
    vkWaitForFences(
        device.get(),
        1,
        &frame.inFlightFence,
        VK_TRUE,
        UINT64_MAX
    );

    vkResetFences(device.get(), 1, &frame.inFlightFence);

    // 2. acquire image
    uint32_t imageIndex;

    vkAcquireNextImageKHR(
        device.get(),
        swapchain.get(),
        UINT64_MAX,
        frame.imageAvailable,
        VK_NULL_HANDLE,
        &imageIndex
    );

    // 3. image ownership sync (CRITICAL)
    VkFence& imageFence = sync.getImageFence(imageIndex);

    if (imageFence != VK_NULL_HANDLE) {
        vkWaitForFences(device.get(), 1, &imageFence, VK_TRUE, UINT64_MAX);
    }

    imageFence = frame.inFlightFence;

    // 4. record command buffer
    vkResetCommandBuffer(commandBuffer, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(commandBuffer, &begin);

    VkClearValue clear{};
    clear.color = {0.2f, 0.3f, 0.6f, 1.0f};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass.get();
    rp.framebuffer = framebuffer.get()[imageIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain.getExtent();
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(commandBuffer);

    vkEndCommandBuffer(commandBuffer);

    // 5. submit
    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;

    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.renderFinished;

    vkQueueSubmit(
        device.getGraphicsQueue(),
        1,
        &submit,
        frame.inFlightFence
    );

    // 6. present
    VkSwapchainKHR swap = swapchain.get();

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swap;
    present.pImageIndices = &imageIndex;

    vkQueuePresentKHR(device.getPresentQueue(), &present);

    // 7. next frame
    sync.advance();
}


void VulkanContext::setupSyncObjects(VkDevice device)
{
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // 关键：第一帧不阻塞

    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphore) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphore) != VK_SUCCESS ||
        vkCreateFence(device, &fenceInfo, nullptr, &inFlightFence) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create sync objects");
    }

    std::cout << "[Vulkan] Sync objects created\n";
}

