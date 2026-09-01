#include "vulkan/frame/FrameRenderer.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/culling/Frustum.h"
#include "vulkan/frame/FrameGraph.h"
#include "imgui.h"

#include <stdexcept>
#include <iostream>
#include <array>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include "vulkan/buffer/UniformBuffer.h"

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

    // =====================================================
    // GPUCulling
    // =====================================================
    int cullingPass = graph->addPass({
        "GPUCullingPass",
        {},
        [this](VkCommandBuffer cmd)
        {  
            // 1. reset instanceCount=0（CPU directe write, HOST_VISIBLE）
            for (int i = 0; i < 3; i++)
            {
                context->lod(i).indirectDrawBuffer.resetInstanceCount(
                    context->device().get(),
                    context->lod(i).indexBuffer.indexCount()
                );
            }

            // 2. update frustum
            glm::mat4 view = context->camera().getViewMatrix();
            glm::mat4 proj = context->camera().getProjectionMatrix();

            glm::vec3 camPos = context->camera().position();
            FrustumPlanes frustum = FrustumPlanes::extractFromMatrix(proj * view, camPos);
            context->frustumBuffer().upload(
                context->device().get(),
                &frustum,
                sizeof(FrustumPlanes)
            );

            // 3. dispatch
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->computePipeline().get());

            VkDescriptorSet ds = context->computeDescriptor().set();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->computePipeline().layout(), 0, 1, &ds, 0, nullptr);

            vkCmdDispatch(cmd, (VulkanContext::OBJECT_COUNT + 63) / 64, 1, 1);
        },
        PassStage::Compute
    });

    // =====================================================
    // Pass 0: Geometry
    // =====================================================
    int geometryPass = graph->addPass(RGPass{
        "GeometryPass",
        {cullingPass},
        [this](VkCommandBuffer cmd)
        {
            // update MVP each frame
            UBOData ubo{};
            // model matrix - accumulated angle (not raw glfwGetTime()) so
            // pausing via VulkanContext::toggleSpinPaused() freezes at the
            // current angle instead of snapping when resumed.
            ubo.model = glm::rotate(
                glm::mat4(1.0f),
                context->spinAngle(),
                glm::vec3(0.0f, 1.0f, 0.0f)
            );
            ubo.view = context->camera().getViewMatrix();
            ubo.proj = context->camera().getProjectionMatrix();

            context->uniformBuffer().update(context->device().get(), ubo);

            // Shared per-frame scene/light data - one upload, read by every
            // material's descriptor set at binding 2 (see SceneData.h).
            SceneData scene{};
            scene.lightDirection = glm::vec4(glm::normalize(context->lightDirection()), 0.0f);
            scene.lightColor     = glm::vec4(context->lightColor(), context->lightIntensity());
            scene.cameraPos      = glm::vec4(context->camera().position(), 1.0f);
            context->sceneDataBuffer().upload(context->device().get(), &scene, sizeof(SceneData));

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipeline().get());

            // bind DescriptorSet（notify where GPU uniform buffer is）
            VkDescriptorSet ds = context->descriptor().set();
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                context->pipeline().getLayout(),
                0, 1, &ds,
                0, nullptr
            );

            // Grid material - rough dielectric. Pushed before the loop since
            // the grid and projectile share this pipeline's push-constant
            // range in the same command buffer and must each set it fresh.
            MaterialPushConstants gridMat{ glm::vec4(1.0f), glm::vec4(0.0f, 0.5f, 0.0f, 0.0f) };
            vkCmdPushConstants(
                cmd, context->pipeline().getLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(MaterialPushConstants), &gridMat
            );

            for (int i = 0; i < 3; i++)
            {
                context->lod(i).vertexBuffer.bind(cmd);
                context->lod(i).indexBuffer.bind(cmd);

                VkBuffer instanceBuf = context->lod(i).visibleInstanceBuffer.get();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuf, &offset);

                vkCmdDrawIndexedIndirect(
                    cmd,
                    context->lod(i).indirectDrawBuffer.get(),
                    0,    // offset
                    1,    // drawCount(ONLY ONE draw command)
                    sizeof(DrawIndirectCommand)
                );
            }

            // Mouse-fired projectile: reuses LOD2's mesh, its own UBO
            // (identity model - no spin) and its own descriptor set/
            // instance buffer, since the grid's UBO/instance buffer are
            // already claimed by the loop above this frame.
            if (context->projectile().isActive())
            {
                UBOData projUbo{};
                projUbo.model = glm::mat4(1.0f);
                projUbo.view  = context->camera().getViewMatrix();
                projUbo.proj  = context->camera().getProjectionMatrix();
                context->projectileUniformBuffer().update(context->device().get(), projUbo);

                InstanceData projInstance{ glm::vec4(context->projectile().position(), 0.0f) };
                context->projectileInstanceBuffer().upload(
                    context->device().get(), &projInstance, sizeof(InstanceData));

                VkDescriptorSet projDs = context->projectileDescriptor().set();
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    context->pipeline().getLayout(),
                    0, 1, &projDs,
                    0, nullptr
                );

                // Shiny metal - visually distinct from the grid's rough
                // dielectric, proving the push constant varies per-draw.
                MaterialPushConstants projMat{ glm::vec4(1.0f), glm::vec4(1.0f, 0.2f, 0.0f, 0.0f) };
                vkCmdPushConstants(
                    cmd, context->pipeline().getLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(MaterialPushConstants), &projMat
                );

                context->lod(2).vertexBuffer.bind(cmd);
                context->lod(2).indexBuffer.bind(cmd);

                VkBuffer projInstanceBuf = context->projectileInstanceBuffer().get();
                VkDeviceSize projOffset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &projInstanceBuf, &projOffset);

                vkCmdDrawIndexed(cmd, context->lod(2).indexBuffer.indexCount(), 1, 0, 0, 0);
            }
        },
        PassStage::Graphics
    });

    // =====================================================
    // Pass 1: Lighting (no pipeline yet → Stage B placeholder)
    // =====================================================
    int lightingPass = graph->addPass(RGPass{
        "LightingPass",
        { geometryPass },
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
        [](VkCommandBuffer cmd)
        {
            // Stage B: placeholder
        }
    });

    // =====================================================
    // ImGuiPass
    // =====================================================
    graph->addPass({
        "ImGuiPass",
        { geometryPass },   // 依赖GeometryPass，确保UI画在最上层
        [this](VkCommandBuffer cmd)
        {
            context->imguiLayer().beginFrame();

            ImGui::Begin("GPU Culling Stats");
            // ImGui::Text("Visible: %u / %u", context->getLastVisibleCount(), VulkanContext::OBJECT_COUNT);
            ImGui::Text("LOD0 (near):   %u", context->getLastVisibleCount(0));
            ImGui::Text("LOD1 (mid):    %u", context->getLastVisibleCount(1));
            ImGui::Text("LOD2 (far):    %u", context->getLastVisibleCount(2));
            ImGui::Text("Total visible: %u / %u",
                context->getLastVisibleCount(0) +
                context->getLastVisibleCount(1) +
                context->getLastVisibleCount(2),
                VulkanContext::OBJECT_COUNT);
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)",
                context->camera().position().x,
                context->camera().position().y,
                context->camera().position().z
            );
            ImGui::End();

            // Interactive lighting tuning - turns "does the lighting look
            // right" into something verifiable in real time rather than a
            // one-time eyeball check.
            ImGui::Begin("Lighting");
            glm::vec3 lightDir = context->lightDirection();
            if (ImGui::SliderFloat3("Direction", &lightDir.x, -1.0f, 1.0f))
                context->setLightDirection(lightDir);

            glm::vec3 lightColor = context->lightColor();
            if (ImGui::ColorEdit3("Color", &lightColor.x))
                context->setLightColor(lightColor);

            float lightIntensity = context->lightIntensity();
            if (ImGui::SliderFloat("Intensity", &lightIntensity, 0.0f, 10.0f))
                context->setLightIntensity(lightIntensity);
            ImGui::End();

            context->imguiLayer().render(cmd);
        },
        PassStage::Graphics
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
    
    // insert into ImGuiPass
    for (int i = 0; i < 3; i++)
    {
        context->setLastVisibleCount(i,
            context->lod(i).indirectDrawBuffer.getVisibleCount(context->device().get())
        );
    }
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

    // ===== Compute：outside RenderPass =====
    graph->executeCompute(frame.commandBuffer);

    // Barrier：insure compute COMPLETE visibility buffer 
    // then let graphics read
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = 
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | 
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

    vkCmdPipelineBarrier(
        frame.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr
    );

    // ===== Graphics：inside RenderPass =====
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
    graph->executeGraphics(frame.commandBuffer);

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

