#include "vulkan/frame/FrameRenderer.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/culling/Frustum.h"
#include "vulkan/frame/FrameGraph.h"
#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* - one-time default dock layout, see ImGuiPass below
#include "imgui_impl_vulkan.h"

#include <stdexcept>
#include <iostream>
#include <array>
#include <cstdint>

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
    createQueryPools();

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
            // Shadow pass's light-frustum-culled instance set - always
            // LOD0's index count, since the shadow pass only ever draws
            // LOD0 geometry (see ShadowPass below).
            context->shadowIndirectDrawBuffer().resetInstanceCount(
                context->device().get(),
                context->lod(0).indexBuffer.indexCount()
            );

            // 2. update frustum
            glm::mat4 view = context->camera().getViewMatrix();
            glm::mat4 proj = context->camera().getProjectionMatrix();

            glm::vec3 camPos = context->camera().position();
            FrustumPlanes frustum = FrustumPlanes::extractFromMatrix(proj * view, camPos);
            frustum.lodDistances = glm::vec4(context->lod1Distance(), context->lod2Distance(), 0.0f, 0.0f);
            context->frustumBuffer().upload(
                context->device().get(),
                &frustum,
                sizeof(FrustumPlanes)
            );

            // Light frustum - same 6-plane extraction, but from the
            // light's orthoRH_ZO view-projection (VulkanContext::
            // lightViewProj()), which uses Vulkan's [0,1] z_ndc
            // convention rather than the camera's default [-1,1] one.
            // zeroToOne=true selects the matching near-plane formula -
            // see Frustum.h. cameraPos/lodDistances are unused by the
            // light-frustum test in culling.comp, left default.
            FrustumPlanes lightFrustum = FrustumPlanes::extractFromMatrix(
                context->lightViewProj(), glm::vec3(0.0f), /*zeroToOne=*/true);
            context->lightFrustumBuffer().upload(
                context->device().get(),
                &lightFrustum,
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
    // ShadowPass - depth-only render from the light's point of view.
    // Draws the grid instances that survive GPUCullingPass's light-
    // frustum test (culling.comp's ShadowVisible/ShadowIndirect output,
    // see VulkanContext::shadowVisibleInstanceBuffer()/
    // shadowIndirectDrawBuffer()) via an indirect draw, same shape as
    // GeometryPass's per-LOD indirect draws. Runs in its own render pass
    // (context->shadowRenderPass()/shadowFramebuffer()), wrapped
    // explicitly in drawFrame() below - not inside the main color+depth
    // render pass.
    // =====================================================
    int shadowPass = graph->addPass(RGPass{
        "ShadowPass",
        {},
        [this](VkCommandBuffer cmd)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->shadowPipeline().get());

            glm::mat4 lightVP = context->lightViewProj();

            // Grid: same accumulated spin rotation as GeometryPass's ubo.model
            // (see below) - the shadow map must be cast from the orientation
            // the mesh is actually rendered at this frame, not its rest pose.
            ShadowPushConstants gridPc{};
            gridPc.lightViewProj = lightVP;
            gridPc.model = glm::rotate(
                glm::mat4(1.0f),
                context->spinAngle(),
                glm::vec3(0.0f, 1.0f, 0.0f)
            );
            vkCmdPushConstants(
                cmd, context->shadowPipeline().layout(), VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(ShadowPushConstants), &gridPc
            );

            context->lod(0).vertexBuffer.bind(cmd);
            context->lod(0).indexBuffer.bind(cmd);

            VkBuffer instanceBuf = context->shadowVisibleInstanceBuffer().get();
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuf, &offset);

            vkCmdDrawIndexedIndirect(
                cmd,
                context->shadowIndirectDrawBuffer().get(),
                0,    // offset
                1,    // drawCount (only one draw command)
                sizeof(DrawIndirectCommand)
            );

            if (context->projectile().isActive())
            {
                // Projectile: identity model, matching GeometryPass's projUbo.
                ShadowPushConstants projPc{};
                projPc.lightViewProj = lightVP;
                projPc.model = glm::mat4(1.0f);
                vkCmdPushConstants(
                    cmd, context->shadowPipeline().layout(), VK_SHADER_STAGE_VERTEX_BIT,
                    0, sizeof(ShadowPushConstants), &projPc
                );

                context->lod(2).vertexBuffer.bind(cmd);
                context->lod(2).indexBuffer.bind(cmd);

                VkBuffer projInstanceBuf = context->projectileInstanceBuffer().get();
                VkDeviceSize projOffset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &projInstanceBuf, &projOffset);

                vkCmdDrawIndexed(cmd, context->lod(2).indexBuffer.indexCount(), 1, 0, 0, 0);
            }
        },
        PassStage::Shadow
    });

    // =====================================================
    // Pass 0: Geometry
    // =====================================================
    int geometryPass = graph->addPass(RGPass{
        "GeometryPass",
        {cullingPass, shadowPass},
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
            scene.lightViewProj  = context->lightViewProj();
            scene.shadowParams   = glm::vec4(context->shadowBias(), 0.0f, 0.0f, 0.0f);
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
    // ImGuiPass - runs in the swapchain's own render pass (PassStage::UI),
    // separate from the offscreen scene pass above. Docked layout: a
    // central "Viewport" panel shows the scene (sceneViewportSet_, sampling
    // sceneColorTarget_) instead of the debug windows overlapping it
    // directly, per the Phase 11 "extra area, not the main screen" change.
    // =====================================================
    graph->addPass({
        "ImGuiPass",
        { geometryPass },   // documents ordering; PassStage separation is what actually enforces it
        [this](VkCommandBuffer cmd)
        {
            context->imguiLayer().beginFrame();

            ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
            ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

            if (!dockLayoutInitialized_)
            {
                dockLayoutInitialized_ = true;

                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                ImGuiID dockMain = dockspaceId;
                ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.28f, nullptr, &dockMain);

                ImGui::DockBuilderDockWindow("Viewport", dockMain);
                ImGui::DockBuilderDockWindow("GPU Culling Stats", dockRight);
                ImGui::DockBuilderDockWindow("Lighting", dockRight);
                ImGui::DockBuilderDockWindow("Shadow Map", dockRight);

                ImGui::DockBuilderFinish(dockspaceId);
            }

            // The live 3D scene, rendered offscreen by GeometryPass above -
            // this is the "extra area" change: debug windows dock beside
            // this instead of floating on top of the rendered grid.
            ImGui::Begin("Viewport");
            ImGui::Image((ImTextureID)(intptr_t)sceneViewportSet_, ImGui::GetContentRegionAvail());
            ImGui::End();

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

            // LOD distance thresholds - runtime-tunable instead of
            // culling.comp's former hardcoded LOD1_DIST/LOD2_DIST
            // constants (see docs/TECHNICAL_NOTES.md). Setters keep
            // LOD2 >= LOD1 so the shader's if/else-if chain stays sane.
            ImGui::Separator();
            ImGui::Text("LOD Thresholds (distance)");
            float lod1Dist = context->lod1Distance();
            if (ImGui::SliderFloat("LOD1 Distance", &lod1Dist, 1.0f, 60.0f, "%.1f"))
                context->setLod1Distance(lod1Dist);

            float lod2Dist = context->lod2Distance();
            if (ImGui::SliderFloat("LOD2 Distance", &lod2Dist, 1.0f, 60.0f, "%.1f"))
                context->setLod2Distance(lod2Dist);

            ImGui::Separator();
            ImGui::Text("GPU Timing (ms)");
            if (context->device().supportsTimestampQueries())
            {
                const auto& timing = context->gpuTiming();
                ImGui::Text("Culling (Compute): %.3f", timing.cullingMs);
                ImGui::Text("Shadow Pass:       %.3f", timing.shadowMs);
                ImGui::Text("Graphics (Geo+UI): %.3f", timing.graphicsMs);
                ImGui::Text("Total GPU:         %.3f", timing.totalMs);
            }
            else
            {
                ImGui::Text("N/A - GPU does not support timestamp queries");
            }
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

            float shadowBias = context->shadowBias();
            if (ImGui::SliderFloat("Shadow Bias", &shadowBias, 0.0001f, 0.02f, "%.4f"))
                context->setShadowBias(shadowBias);
            ImGui::End();

            // Milestone 1 verification: the grid's silhouette (from the
            // light's point of view) should be visible here and rotate
            // with the Direction slider above, before any shading code
            // depends on the shadow map (see docs/TECHNICAL_NOTES.md).
            ImGui::Begin("Shadow Map");
            ImGui::Image((ImTextureID)(intptr_t)shadowMapDebugSet_, ImVec2(256.0f, 256.0f));
            ImGui::End();

            context->imguiLayer().render(cmd);
        },
        PassStage::UI
    });

    // =====================================================
    // Build DAG
    // =====================================================
    graph->build();

    // Debug-only: register the shadow map with ImGui so the "Shadow Map"
    // window (ImGuiPass, above) can preview it - Milestone 1's
    // verification step, before any shading code samples it.
    shadowMapDebugSet_ = ImGui_ImplVulkan_AddTexture(
        context->shadowMap().view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Registers the offscreen scene color target so the "Viewport" window
    // (ImGuiPass, above) can display the live 3D scene - same mechanism as
    // the shadow map preview just above, aimed at the color target instead.
    sceneViewportSet_ = ImGui_ImplVulkan_AddTexture(
        context->sceneColorTarget().view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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

    // GPU timing: this frame slot's fence wait above already guarantees
    // its previous use finished, so its query pool results are ready -
    // same safe-readback reasoning as the LOD counts just above. Skipped
    // until this slot has actually been written to once (frameCounter_ <
    // frames.size() means this is one of the first MAX_FRAMES calls).
    if (context->device().supportsTimestampQueries() && frameCounter_ >= frames.size())
    {
        uint64_t timestamps[4];
        VkResult qr = vkGetQueryPoolResults(
            device.get(), frame.queryPool, 0, 4,
            sizeof(timestamps), timestamps, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT
        );

        if (qr == VK_SUCCESS)
        {
            float toMs = context->device().timestampPeriodNs() / 1e6f;
            VulkanContext::GpuTiming timing{};
            timing.cullingMs  = (timestamps[1] - timestamps[0]) * toMs;
            timing.shadowMs   = (timestamps[2] - timestamps[1]) * toMs;
            timing.graphicsMs = (timestamps[3] - timestamps[2]) * toMs;
            timing.totalMs    = (timestamps[3] - timestamps[0]) * toMs;
            context->setGpuTiming(timing);
        }
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

    bool timingEnabled = frame.queryPool != VK_NULL_HANDLE;
    if (timingEnabled)
    {
        // Must reset before first use in this recording, and outside any
        // render pass - both true here, right at the top of the buffer.
        vkCmdResetQueryPool(frame.commandBuffer, frame.queryPool, 0, 4);
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.queryPool, 0);
    }

    // ===== Compute：outside RenderPass =====
    graph->executeCompute(frame.commandBuffer);

    if (timingEnabled)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool, 1);

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

    // ===== Shadow：its own render pass, fixed-size shadow map target =====
    auto& shadowMap = context->shadowMap();

    VkClearValue shadowClear{};
    shadowClear.depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo shadowRp{};
    shadowRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    shadowRp.renderPass = context->shadowRenderPass().get();
    shadowRp.framebuffer = context->shadowFramebuffer().get()[0];
    shadowRp.renderArea.offset = {0, 0};
    shadowRp.renderArea.extent = shadowMap.extent();
    shadowRp.clearValueCount = 1;
    shadowRp.pClearValues = &shadowClear;

    vkCmdBeginRenderPass(frame.commandBuffer, &shadowRp, VK_SUBPASS_CONTENTS_INLINE);
    graph->executeShadow(frame.commandBuffer);
    vkCmdEndRenderPass(frame.commandBuffer);

    if (timingEnabled)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool, 2);

    // Barrier: the shadow pass's depth write must finish (and become
    // visible) before the main render pass's fragment shader samples it
    // (ImGui's debug preview this milestone; the geometry pass itself
    // from Milestone 2 onward). The render pass's finalLayout already
    // transitions the image to SHADER_READ_ONLY_OPTIMAL - this barrier
    // only orders the memory access, it isn't a layout transition.
    VkImageMemoryBarrier shadowBarrier{};
    shadowBarrier.sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    shadowBarrier.oldLayout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowBarrier.newLayout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowBarrier.image             = shadowMap.image();
    shadowBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    shadowBarrier.subresourceRange.baseMipLevel   = 0;
    shadowBarrier.subresourceRange.levelCount     = 1;
    shadowBarrier.subresourceRange.baseArrayLayer = 0;
    shadowBarrier.subresourceRange.layerCount     = 1;
    shadowBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadowBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        frame.commandBuffer,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &shadowBarrier
    );

    // ===== Graphics: offscreen scene pass, its own render pass/framebuffer
    // (sceneRenderPass_/sceneFramebuffer_), fixed resolution - sampled by
    // ImGui's "Viewport" window below instead of being presented directly.
    auto& sceneColorTarget = context->sceneColorTarget();

    std::array<VkClearValue, 2> sceneClearValues{};
    sceneClearValues[0].color        = { 0.1f, 0.2f, 0.7f, 1.0f };
    sceneClearValues[1].depthStencil = { 1.0f, 0 };   // depth=1.0（最远），stencil=0

    VkRenderPassBeginInfo sceneRp{};
    sceneRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    sceneRp.renderPass = context->sceneRenderPass().get();
    sceneRp.framebuffer = context->sceneFramebuffer().get()[0];
    sceneRp.renderArea.offset = {0, 0};
    sceneRp.renderArea.extent = sceneColorTarget.extent();
    sceneRp.clearValueCount = static_cast<uint32_t>(sceneClearValues.size());
    sceneRp.pClearValues    = sceneClearValues.data();

    vkCmdBeginRenderPass(frame.commandBuffer, &sceneRp, VK_SUBPASS_CONTENTS_INLINE);
    graph->executeGraphics(frame.commandBuffer);
    vkCmdEndRenderPass(frame.commandBuffer);

    // Barrier: the scene pass's color write must finish (and become
    // visible) before ImGui's fragment shader samples it in the Viewport
    // window below - same shape as the shadow barrier above, generalized
    // to the color attachment instead of depth (see architecture.md).
    VkImageMemoryBarrier sceneBarrier{};
    sceneBarrier.sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sceneBarrier.oldLayout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.newLayout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.image             = sceneColorTarget.image();
    sceneBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    sceneBarrier.subresourceRange.baseMipLevel   = 0;
    sceneBarrier.subresourceRange.levelCount     = 1;
    sceneBarrier.subresourceRange.baseArrayLayer = 0;
    sceneBarrier.subresourceRange.layerCount     = 1;
    sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        frame.commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &sceneBarrier
    );

    // ===== UI: swapchain's own render pass, ImGui only (PassStage::UI) =====
    std::array<VkClearValue, 2> uiClearValues{};
    uiClearValues[0].color        = { 0.06f, 0.06f, 0.07f, 1.0f };   // editor-style background behind docked windows
    uiClearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass.get();
    rp.framebuffer = framebuffer.get()[imageIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain.getExtent();
    rp.clearValueCount = static_cast<uint32_t>(uiClearValues.size());
    rp.pClearValues    = uiClearValues.data();

    vkCmdBeginRenderPass(frame.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    graph->executeUI(frame.commandBuffer);

    vkCmdEndRenderPass(frame.commandBuffer);

    if (timingEnabled)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool, 3);

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
    frameCounter_++;
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

        if (frame.queryPool != VK_NULL_HANDLE)
            vkDestroyQueryPool(device, frame.queryPool, nullptr);
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

void FrameRenderer::createQueryPools()
{
    if (!context->device().supportsTimestampQueries())
        return;   // frame.queryPool stays VK_NULL_HANDLE - drawFrame()/cleanup() skip it

    VkDevice device = context->device().get();

    VkQueryPoolCreateInfo info{};
    info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 4;   // frame start, compute end, shadow end, graphics end

    for (auto& frame : frames)
    {
        if (vkCreateQueryPool(device, &info, nullptr, &frame.queryPool) != VK_SUCCESS)
            throw std::runtime_error("Failed to create timestamp query pool");
    }
}

