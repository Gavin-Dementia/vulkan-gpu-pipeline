#include "vulkan/frame/FrameRenderer.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/culling/Frustum.h"
#include "vulkan/frame/FrameGraph.h"
#include "vulkan/pipeline/VulkanSkyboxPipeline.h"
#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* - one-time default dock layout, see ImGuiPass below
#include "imgui_impl_vulkan.h"

#include <stdexcept>
#include <iostream>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>

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
            VkExtent2D sceneExtent = context->sceneColorTarget().extent();
            float aspectRatio = static_cast<float>(sceneExtent.width) / static_cast<float>(sceneExtent.height);

            glm::mat4 view = context->camera().getViewMatrix();
            glm::mat4 proj = context->camera().getProjectionMatrix(aspectRatio);

            glm::vec3 camPos = context->camera().position();
            FrustumPlanes frustum = FrustumPlanes::extractFromMatrix(proj * view, camPos);

            // Screen-space projection scale: pixels-per-world-unit at
            // distance 1 from the camera, along the vertical FOV. A
            // bounding sphere of radius r at distance d projects to
            // roughly r*screenScale/d pixels (small-angle approximation) -
            // culling.comp uses this to compare an object's on-screen size
            // against lod1ScreenSize()/lod2ScreenSize() instead of a flat
            // world-space distance, so the same thresholds stay meaningful
            // regardless of FOV or output resolution. Derived from the
            // same Camera::FOV_DEGREES getProjectionMatrix() already uses
            // (single source of truth) and the scene render target's
            // *current* height (see docs/TECHNICAL_NOTES.md §36 - the
            // offscreen scene pass, not the swapchain, is what's actually
            // rasterized, and its resolution is no longer fixed).
            float screenScale = static_cast<float>(sceneExtent.height)
                / (2.0f * glm::tan(glm::radians(Camera::FOV_DEGREES) * 0.5f));
            frustum.lodParams = glm::vec4(
                context->lod1ScreenSize(), context->lod2ScreenSize(), screenScale, 0.0f);
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
            // see Frustum.h. cameraPos/lodParams are unused by the
            // light-frustum test in culling.comp, left default.
            FrustumPlanes lightFrustum = FrustumPlanes::extractFromMatrix(
                context->lightViewProj(), glm::vec3(0.0f), /*zeroToOne=*/true);
            context->lightFrustumBuffer().upload(
                context->device().get(),
                &lightFrustum,
                sizeof(FrustumPlanes)
            );

            // 3. dispatch - two stages: a coarse pass over the 64
            // clusters, then the existing fine per-object pass, which
            // reads the coarse pass's per-cluster visibility flags to
            // skip the per-object plane test for clusters already known
            // to be fully outside a frustum. Both share computeDescriptor_'s
            // single descriptor set (see ComputeDescriptor.cpp) - only the
            // bound pipeline changes between the two dispatches.
            VkDescriptorSet ds = context->computeDescriptor().set();

            // 3a. Coarse: one workgroup, one thread per cluster
            // (CLUSTER_COUNT == 64 == local_size_x, so a single (1,1,1)
            // dispatch covers every cluster exactly).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->computePipelineCoarse().get());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->computePipelineCoarse().layout(), 0, 1, &ds, 0, nullptr);
            vkCmdDispatch(cmd, 1, 1, 1);

            // Compute->compute barrier: the fine pass below reads the
            // cluster visibility flags this dispatch just wrote. Without
            // this, the fine pass could read stale/undefined flags - a
            // race, not a crash, so silent nondeterministic over/under-
            // culling rather than a validation error. Same blanket,
            // hand-written shape as every other barrier in this function;
            // this is just the first one between two compute dispatches
            // instead of compute->graphics.
            VkMemoryBarrier clusterBarrier{};
            clusterBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            clusterBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            clusterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &clusterBarrier, 0, nullptr, 0, nullptr
            );

            // 3b. Fine: unchanged per-object pass, now cluster-gated.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->computePipeline().get());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->computePipeline().layout(), 0, 1, &ds, 0, nullptr);
            vkCmdDispatch(cmd, (VulkanContext::OBJECT_COUNT + 63) / 64, 1, 1);

            // 3c. Transparency sort (see docs/TECHNICAL_NOTES.md §43) -
            // only dispatched when it could actually matter: gridAlpha()
            // selects the transparent pipeline, and the toggle is on
            // (default). Skipped entirely for the default opaque case, so
            // this is a zero-cost no-op unless transparency is in use.
            if (context->transparencySortEnabled() && context->isTransparent())
            {
                // Same compute->compute barrier shape as the coarse->fine
                // one above - sortInstances.comp reads and rewrites the
                // fine pass's VisibleLODN/IndirectLODN output, so it must
                // not start until those writes are visible.
                VkMemoryBarrier fineBarrier{};
                fineBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                fineBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                fineBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &fineBarrier, 0, nullptr, 0, nullptr
                );

                // One workgroup per LOD bucket (gl_WorkGroupID.x selects
                // which) - see sortInstances.comp.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->sortPipeline().get());
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, context->sortPipeline().layout(), 0, 1, &ds, 0, nullptr);
                vkCmdDispatch(cmd, 3, 1, 1);
            }
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
            // Recomputed fresh every frame, not cached - the scene target's
            // aspect ratio can change at runtime (docked Viewport panel
            // resize, see docs/TECHNICAL_NOTES.md §36).
            VkExtent2D sceneExtent = context->sceneColorTarget().extent();
            float aspectRatio = static_cast<float>(sceneExtent.width) / static_cast<float>(sceneExtent.height);

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
            ubo.proj = context->camera().getProjectionMatrix(aspectRatio);

            context->uniformBuffer().update(context->device().get(), ubo);

            // Shared per-frame scene/light data - one upload, read by every
            // material's descriptor set at binding 2 (see SceneData.h).
            SceneData scene{};
            scene.lightDirection = glm::vec4(glm::normalize(context->lightDirection()), 0.0f);
            scene.lightColor     = glm::vec4(context->lightColor(), context->lightIntensity());
            scene.cameraPos      = glm::vec4(context->camera().position(), 1.0f);
            scene.lightViewProj  = context->lightViewProj();
            // y/z = sceneColorTarget_'s extent (Phase 23 M1) - see
            // SceneData.h; triangle_refractive.frag divides gl_FragCoord.xy
            // by this to recover its screen UV.
            scene.shadowParams   = glm::vec4(
                context->shadowBias(),
                static_cast<float>(sceneExtent.width),
                static_cast<float>(sceneExtent.height),
                0.0f
            );
            context->sceneDataBuffer().upload(context->device().get(), &scene, sizeof(SceneData));

            // Skybox - drawn first so the grid's normal LESS depth test
            // naturally overdraws it wherever real geometry exists; depth
            // write disabled so it never pollutes the depth buffer the
            // grid's test depends on. No new FrameGraph pass - same "just
            // another draw call in this lambda" precedent the projectile
            // draw below already uses. See docs/TECHNICAL_NOTES.md (IBL
            // Milestone 1).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->skyboxPipeline().get());
            VkDescriptorSet skyDs = context->skyboxDescriptor().set();
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                context->skyboxPipeline().layout(), 0, 1, &skyDs, 0, nullptr
            );

            SkyboxPushConstants skyPc{};
            skyPc.invViewProj = glm::inverse(ubo.proj * ubo.view);
            skyPc.cameraPos   = glm::vec4(context->camera().position(), 1.0f);
            vkCmdPushConstants(
                cmd, context->skyboxPipeline().layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SkyboxPushConstants), &skyPc
            );
            vkCmdDraw(cmd, 3, 1, 0, 0);

            // Alpha-blended sibling of pipeline_ once gridAlpha() < 1.0
            // (see docs/TECHNICAL_NOTES.md §43) - same shaders/layout, so
            // every descriptor bind/push-constant call below reads
            // activePipeline.getLayout() and stays correct either way.
            // Refraction (Phase 23 M1) takes priority over gridAlpha() when
            // both are set - it's an opaque-depth pipeline (see
            // refractivePipeline_'s creation comment), not the alpha-blend
            // one, so `transparent` here only ever selects
            // transparentPipeline_, never both at once.
            bool refraction = context->refractionEnabled();
            bool transparent = !refraction && context->isTransparent();
            VulkanPipeline& activePipeline = refraction ? context->refractivePipeline()
                : transparent ? context->transparentPipeline() : context->pipeline();

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline.get());

            // IBL Milestones 2-3 (see docs/TECHNICAL_NOTES.md §34/§35):
            // set 1, ambient-lighting data (diffuse irradiance + specular
            // prefilter + BRDF LUT) shared by every material - bound once
            // per frame here, stays bound across the grid's and
            // projectile's later set-0-only rebinds below (Vulkan's
            // descriptor-set binding-persistence rule).
            VkDescriptorSet iblDs = context->iblDescriptor().set();
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                activePipeline.getLayout(),
                1, 1, &iblDs,
                0, nullptr
            );

            // bind DescriptorSet（notify where GPU uniform buffer is）
            VkDescriptorSet ds = context->descriptor().set();
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                activePipeline.getLayout(),
                0, 1, &ds,
                0, nullptr
            );

            // Set 2 (Phase 23 M1) - the previous frame's captured scene
            // color, only meaningful (and only present in
            // refractivePipeline_'s layout) when refraction is active.
            if (refraction)
            {
                VkDescriptorSet refractDs = context->refractionDescriptor().set();
                vkCmdBindDescriptorSets(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline.getLayout(),
                    2, 1, &refractDs, 0, nullptr
                );
            }

            // Grid material - rough dielectric. Pushed before the loop since
            // the grid and projectile share this pipeline's push-constant
            // range in the same command buffer and must each set it fresh.
            // Alpha (w) is gridAlpha() - 1.0 (opaque) by default, so this
            // is byte-identical to the pre-§43 behavior unless transparency
            // is actually in use. metallicRoughness.z is the master texture
            // toggle (§44); .w is IOR (Phase 23 M1, refractivePipeline_
            // only - triangle.frag never reads it).
            MaterialPushConstants gridMat{
                glm::vec4(1.0f, 1.0f, 1.0f, context->gridAlpha()),
                glm::vec4(0.0f, 0.5f, context->texturesEnabled() ? 1.0f : 0.0f, context->refractionIOR()) };
            vkCmdPushConstants(
                cmd, activePipeline.getLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(MaterialPushConstants), &gridMat
            );

            // Bucket draw order: nearest-first (0,1,2) is better for
            // opaque early-z rejection; back-to-front (2,1,0) is required
            // for correct alpha blending once transparent - see
            // sortInstances.comp for why this order is already correct
            // *between* buckets (LOD0's camera-distance range is strictly
            // less than LOD1's, which is strictly less than LOD2's, by
            // the lod1ScreenSize_ >= lod2ScreenSize_ invariant), leaving
            // only *within*-bucket order for that shader to fix.
            for (int step = 0; step < 3; step++)
            {
                int i = transparent ? (2 - step) : step;

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
                projUbo.proj  = context->camera().getProjectionMatrix(aspectRatio);
                context->projectileUniformBuffer().update(context->device().get(), projUbo);

                InstanceData projInstance{ glm::vec4(context->projectile().position(), 0.0f) };
                context->projectileInstanceBuffer().upload(
                    context->device().get(), &projInstance, sizeof(InstanceData));

                VkDescriptorSet projDs = context->projectileDescriptor().set();
                vkCmdBindDescriptorSets(
                    cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    activePipeline.getLayout(),
                    0, 1, &projDs,
                    0, nullptr
                );

                // Shiny metal - visually distinct from the grid's rough
                // dielectric, proving the push constant varies per-draw.
                // Shares gridAlpha() rather than its own slider - this
                // scoped fix treats "transparent" as one shared material
                // toggle, not per-object (see docs/TECHNICAL_NOTES.md §43).
                // Note this draw is still issued after the grid loop
                // unconditionally, so its blend order relative to the grid
                // isn't sorted - a known limitation, not addressed here.
                MaterialPushConstants projMat{
                    glm::vec4(1.0f, 1.0f, 1.0f, context->gridAlpha()),
                    glm::vec4(1.0f, 0.2f, context->texturesEnabled() ? 1.0f : 0.0f, context->refractionIOR()) };
                vkCmdPushConstants(
                    cmd, activePipeline.getLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
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

            // Live-resized viewport target (see docs/TECHNICAL_NOTES.md
            // §36): detect the docked panel's current pixel size and, if
            // it differs from the offscreen target's, queue a resize -
            // applied at the top of the *next* frame (see drawFrame()),
            // not here, since this frame's GeometryPass has already
            // recorded draws against the current sceneFramebuffer_/
            // pipeline_ by the time ImGuiPass runs. No debounce: while
            // the panel border is actively being dragged, this can queue
            // (and drawFrame() apply) a resize on consecutive frames -
            // a real, accepted stall/hitch during an active drag, traded
            // for keeping this path simple (see resizeSceneTarget()'s
            // own comment).
            ImVec2 viewportAvail = ImGui::GetContentRegionAvail();
            VkExtent2D sceneExtent = context->sceneColorTarget().extent();
            uint32_t availWidth  = static_cast<uint32_t>(std::max(viewportAvail.x, 1.0f));
            uint32_t availHeight = static_cast<uint32_t>(std::max(viewportAvail.y, 1.0f));
            if (availWidth != sceneExtent.width || availHeight != sceneExtent.height)
            {
                resizePending_ = true;
                pendingWidth_  = availWidth;
                pendingHeight_ = availHeight;
            }

            ImGui::Image((ImTextureID)(intptr_t)sceneViewportSet_, viewportAvail);
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

            // Hierarchical (coarse + fine) culling: how many of the 64
            // clusters the coarse pass rejected before the fine per-object
            // pass ran - drag the camera until a whole edge cluster leaves
            // the frustum and this count should drop by 1.
            ImGui::Separator();
            ImGui::Text("Clusters visible (camera): %u / %u",
                context->getLastClusterVisibleCamera(), VulkanContext::CLUSTER_COUNT);
            ImGui::Text("Clusters visible (light):   %u / %u",
                context->getLastClusterVisibleLight(), VulkanContext::CLUSTER_COUNT);

            // LOD thresholds - screen-space projected size (px), not a
            // flat world-space distance (see docs/TECHNICAL_NOTES.md for
            // why: a flat distance pair means the same threshold implies
            // a different apparent size at a different FOV/resolution,
            // where a screen-size threshold stays meaningful). Setters
            // keep LOD1 >= LOD2 so the shader's if/else-if chain stays
            // sane (screen size shrinks with distance, the inverse of the
            // old distance-based invariant).
            ImGui::Separator();
            ImGui::Text("LOD Thresholds (screen size, px)");
            float lod1Size = context->lod1ScreenSize();
            if (ImGui::SliderFloat("LOD1 Screen Size", &lod1Size, 1.0f, 400.0f, "%.1f px"))
                context->setLod1ScreenSize(lod1Size);

            float lod2Size = context->lod2ScreenSize();
            if (ImGui::SliderFloat("LOD2 Screen Size", &lod2Size, 1.0f, 400.0f, "%.1f px"))
                context->setLod2ScreenSize(lod2Size);

            // Mesh-detail-derived LOD2 default (see docs/TECHNICAL_NOTES.md
            // §40) - shows the actual triangle counts behind lod2DetailRatio()
            // so the ratio driving the default above isn't just an opaque
            // number, plus a way back to it after dragging the slider.
            ImGui::Text("Mesh detail: LOD0 %u tris, LOD1 %u tris, LOD2 %u tris",
                context->lod0TriangleCount(),
                context->lod1TriangleCount(),
                context->lod2TriangleCount());
            ImGui::Text("LOD2/LOD1 detail ratio: %.1f%%", context->lod2DetailRatio() * 100.0f);
            if (ImGui::Button("Reset LOD2 to mesh-derived default"))
                context->resetLod2ScreenSizeToMeshDefault();

            // Mutual-collision bounciness (TECHNICAL_NOTES.md §30) -
            // runtime-tunable for the same "find the feel by eye" reason
            // as the LOD thresholds above. 0 = instances stop dead on
            // contact, 1 = fully elastic bounce.
            ImGui::Separator();
            ImGui::Text("Collision");
            float restitution = context->restitution();
            if (ImGui::SliderFloat("Restitution", &restitution, 0.0f, 1.0f, "%.2f"))
                context->setRestitution(restitution);

            // Interactive deltaTime clamp (see docs/TECHNICAL_NOTES.md
            // section 37) - off by default, preserving this project's
            // original uncapped-deltaTime behavior. Doubles as a live
            // demonstration of section 37's diagnosis: drag Restitution to
            // 1.0, resize the Viewport panel to trigger a vkDeviceWaitIdle
            // stall, then toggle this on/off to see the difference a
            // clamped vs. unclamped deltaTime spike makes to the
            // resulting collision response.
            ImGui::Separator();
            ImGui::Text("Simulation Timing");
            bool clampDt = context->clampDeltaTimeEnabled();
            if (ImGui::Checkbox("Clamp Delta Time", &clampDt))
                context->setClampDeltaTimeEnabled(clampDt);
            if (clampDt)
            {
                float maxDt = context->maxDeltaTime();
                if (ImGui::SliderFloat("Max Delta Time (s)", &maxDt, 1.0f / 60.0f, 0.5f, "%.3f"))
                    context->setMaxDeltaTime(maxDt);
            }

            // Master texture toggle (§44) - off by default, reproducing
            // Phase 8 milestone 1's flat, push-constant-only PBR shading
            // (no material texture detail, but still real lighting/
            // shadows/IBL). Lets the improvement Phase 8 milestone 2/
            // Phase 20 made be demonstrated on demand instead of only
            // ever seen as the permanent default.
            ImGui::Separator();
            ImGui::Text("Material");
            bool texturesEnabled = context->texturesEnabled();
            if (ImGui::Checkbox("Enable Textures", &texturesEnabled))
                context->setTexturesEnabled(texturesEnabled);

            // Alpha-blended grid/projectile material (see
            // docs/TECHNICAL_NOTES.md §43) - dragging Grid Alpha below 1.0
            // switches GeometryPass onto transparentPipeline_ and reverses
            // the LOD draw order for correct back-to-front blending.
            // "Enable Transparency Sort" only matters once alpha < 1.0 -
            // turn it off to see the blending-order bug it fixes (nearby
            // grid instances overlapping incorrectly), same demonstrate-
            // the-bug pattern as Clamp Delta Time above.
            ImGui::Separator();
            ImGui::Text("Transparency");
            float gridAlpha = context->gridAlpha();
            if (ImGui::SliderFloat("Grid Alpha", &gridAlpha, 0.0f, 1.0f, "%.2f"))
                context->setGridAlpha(gridAlpha);
            bool sortEnabled = context->transparencySortEnabled();
            if (ImGui::Checkbox("Enable Transparency Sort", &sortEnabled))
                context->setTransparencySortEnabled(sortEnabled);

            // Phase 23 M1 (docs/roadmap.md) - global refraction toggle,
            // takes priority over Grid Alpha above when both are set (see
            // GeometryPass's pipeline selection). IOR ~1.33 = water,
            // ~1.5 = glass.
            bool refractionEnabled = context->refractionEnabled();
            if (ImGui::Checkbox("Enable Refraction (glass/jelly)", &refractionEnabled))
                context->setRefractionEnabled(refractionEnabled);
            float ior = context->refractionIOR();
            if (ImGui::SliderFloat("IOR", &ior, 1.0f, 2.5f, "%.2f"))
                context->setRefractionIOR(ior);

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

    // Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39):
    // checked before anything else this frame, same "apply at the very
    // top of the next frame" timing as the scene-target resize below -
    // recreateSwapchainResources() does its own vkDeviceWaitIdle, which
    // subsumes the per-slot fence wait a few lines down. Two independent
    // triggers: a direct framebuffer-size comparison (catches an ordinary
    // window drag - GLFW's framebuffer size already reflects the new size
    // by the time glfwPollEvents() returns in Application::mainLoop()) and
    // swapchainNeedsRecreate_ (set below when vkAcquireNextImageKHR/
    // vkQueuePresentKHR themselves reported the swapchain suboptimal/out
    // of date - e.g. a display mode change, not just a resize).
    {
        int fbWidth, fbHeight;
        glfwGetFramebufferSize(context->window(), &fbWidth, &fbHeight);
        VkExtent2D currentExtent = swapchain.getExtent();
        bool sizeMismatch =
            static_cast<uint32_t>(fbWidth)  != currentExtent.width ||
            static_cast<uint32_t>(fbHeight) != currentExtent.height;

        if (swapchainNeedsRecreate_ || sizeMismatch)
        {
            recreateSwapchainResources();
            swapchainNeedsRecreate_ = false;
        }
    }

    // Live-resized viewport target (see docs/TECHNICAL_NOTES.md §36):
    // apply a resize queued by the *previous* frame's ImGuiPass here, at
    // the very top of the next frame, before any per-frame-slot state is
    // touched - resizeSceneTarget() does its own vkDeviceWaitIdle, which
    // subsumes the per-slot fence wait below. sceneViewportSet_ (the
    // ImGui-registered texture ID for the "Viewport" window) has to be
    // re-registered afterward: it was bound to the old, now-destroyed
    // VkImageView, and ImGui has no way to know that image view changed
    // out from under it otherwise.
    if (resizePending_)
    {
        context->resizeSceneTarget(pendingWidth_, pendingHeight_);
        ImGui_ImplVulkan_RemoveTexture(sceneViewportSet_);
        sceneViewportSet_ = ImGui_ImplVulkan_AddTexture(
            context->sceneColorTarget().view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        resizePending_ = false;
    }

    FrameContext& frame = frames[currentFrame];

    vkWaitForFences(device.get(), 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
    
    // insert into ImGuiPass
    for (int i = 0; i < 3; i++)
    {
        context->setLastVisibleCount(i,
            context->lod(i).indirectDrawBuffer.getVisibleCount(context->device().get())
        );
    }

    // Hierarchical culling stats: same safe post-fence-wait readback as
    // the LOD counts above - this frame slot's prior GPU work is
    // guaranteed done by the fence wait already completed above.
    {
        std::array<uint32_t, VulkanContext::CLUSTER_COUNT> camFlags{};
        std::array<uint32_t, VulkanContext::CLUSTER_COUNT> lightFlags{};
        context->clusterVisibleCameraBuffer().download(
            context->device().get(), camFlags.data(), sizeof(camFlags));
        context->clusterVisibleLightBuffer().download(
            context->device().get(), lightFlags.data(), sizeof(lightFlags));

        uint32_t camCount = 0, lightCount = 0;
        for (uint32_t f : camFlags)   camCount   += f;
        for (uint32_t f : lightFlags) lightCount += f;

        context->setLastClusterVisibleCounts(camCount, lightCount);
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

    // Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39):
    // frame.inFlightFence is deliberately NOT reset until after this
    // acquire succeeds. If it were reset first and the swapchain then
    // turned out to be out of date, drawFrame() would have to bail out
    // without ever submitting - leaving the fence reset-but-never-
    // signaled, so the *next* call's vkWaitForFences() on this same slot
    // would block forever. Waiting on the still-signaled fence from this
    // slot's last real submit costs nothing (it's already signaled).
    uint32_t imageIndex;
    VkResult acquireResult = vkAcquireNextImageKHR(
        device.get(),
        swapchain.getSwapchain(),
        UINT64_MAX,
        frame.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // The image index this call returned (if any) isn't valid to
        // render into - recreate now and pick the resize up cleanly on
        // the *next* drawFrame() call instead of this one.
        recreateSwapchainResources();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Failed to acquire swapchain image");
    if (acquireResult == VK_SUBOPTIMAL_KHR)
        swapchainNeedsRecreate_ = true;   // still a valid image this frame - recreate next frame instead

    vkResetFences(device.get(), 1, &frame.inFlightFence);

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

    // ===== Phase 23 M1 (docs/roadmap.md): capture the *previous* frame's
    // fully-composited scene into sceneColorCopy_ before this frame clears
    // and redraws sceneColorTarget_ - the source refractive materials
    // sample this frame (see triangle_refractive.frag). Gated on
    // refractionEnabled() (skip the cost entirely when unused, same
    // pattern as the transparency sort dispatch) and
    // sceneColorEverRendered() (skip the one frame right after init/resize
    // where sceneColorTarget_'s actual layout doesn't match what this
    // barrier assumes - see that accessor's comment).
    if (context->refractionEnabled() && context->sceneColorEverRendered())
    {
        auto& srcTarget = context->sceneColorTarget();
        auto& dstCopy   = context->sceneColorCopy();

        std::array<VkImageMemoryBarrier, 2> preCopyBarriers{};
        preCopyBarriers[0].sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        preCopyBarriers[0].oldLayout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        preCopyBarriers[0].newLayout         = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        preCopyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBarriers[0].image             = srcTarget.image();
        preCopyBarriers[0].subresourceRange  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        preCopyBarriers[0].srcAccessMask     = VK_ACCESS_SHADER_READ_BIT;
        preCopyBarriers[0].dstAccessMask     = VK_ACCESS_TRANSFER_READ_BIT;

        // oldLayout=UNDEFINED is always valid here (it means "discard
        // whatever's there," not "must currently be UNDEFINED") - simpler
        // than tracking dstCopy's true previous layout across frames,
        // which alternates between never-used and this same barrier's own
        // post-copy SHADER_READ_ONLY_OPTIMAL below.
        preCopyBarriers[1].sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        preCopyBarriers[1].oldLayout         = VK_IMAGE_LAYOUT_UNDEFINED;
        preCopyBarriers[1].newLayout         = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        preCopyBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBarriers[1].image             = dstCopy.image();
        preCopyBarriers[1].subresourceRange  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        preCopyBarriers[1].srcAccessMask     = 0;
        preCopyBarriers[1].dstAccessMask     = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(preCopyBarriers.size()), preCopyBarriers.data()
        );

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copyRegion.extent = { srcTarget.extent().width, srcTarget.extent().height, 1 };

        vkCmdCopyImage(
            frame.commandBuffer,
            srcTarget.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dstCopy.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copyRegion
        );

        // Only dstCopy needs transitioning back - srcTarget's upcoming
        // render pass has initialLayout=UNDEFINED (VulkanRenderPass::
        // createOffscreenColor()), so it doesn't matter what layout the
        // copy above left it in.
        VkImageMemoryBarrier postCopyBarrier{};
        postCopyBarrier.sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        postCopyBarrier.oldLayout         = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        postCopyBarrier.newLayout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        postCopyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCopyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCopyBarrier.image             = dstCopy.image();
        postCopyBarrier.subresourceRange  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        postCopyBarrier.srcAccessMask     = VK_ACCESS_TRANSFER_WRITE_BIT;
        postCopyBarrier.dstAccessMask     = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            frame.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &postCopyBarrier
        );
    }

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
    // Unconditional (not gated on refractionEnabled()) - this just records
    // that sceneColorTarget_ now holds a real rendered frame at its
    // current size, which the *next* frame's refraction copy step above
    // needs to know regardless of whether refraction happens to be on this
    // particular frame.
    context->markSceneColorRendered();

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

    VkResult presentResult = vkQueuePresentKHR(device.getPresentQueue(), &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
        swapchainNeedsRecreate_ = true;   // picked up at the top of the next drawFrame()
    else if (presentResult != VK_SUCCESS)
        throw std::runtime_error("Failed to present swapchain image");

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

// Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39).
// VulkanContext::resizeSwapchain() owns the swapchain/framebuffer/depth-
// buffer themselves; this recreates the two things FrameRenderer keeps
// sized to the swapchain's *image count* (not its per-frame-in-flight
// state, which is independent of the swapchain entirely) and makes sure
// ImGui's Vulkan backend knows if that count changed.
void FrameRenderer::recreateSwapchainResources()
{
    context->resizeSwapchain();

    uint32_t imageCount = getImageCount();

    // Non-owning: each entry just points at whichever in-flight frame's
    // fence last used that swapchain image, so a plain resize/reset (no
    // destroy) is correct here even when imageCount changed.
    imagesInFlight.assign(imageCount, VK_NULL_HANDLE);

    // Per-swapchain-image semaphores, unlike imagesInFlight, are real
    // owned objects - only recreate them if the image count actually
    // changed (the common case, a plain resize, keeps the same count).
    if (imageRenderFinished.size() != imageCount)
    {
        VkDevice device = context->device().get();
        for (VkSemaphore sem : imageRenderFinished)
            vkDestroySemaphore(device, sem, nullptr);

        imageRenderFinished.assign(imageCount, VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : imageRenderFinished)
        {
            if (vkCreateSemaphore(device, &semInfo, nullptr, &sem) != VK_SUCCESS)
                throw std::runtime_error("Failed to recreate imageRenderFinished semaphore");
        }
    }

    // Only matters if imageCount changed, but harmless (and required by
    // ImGui's own contract) to call unconditionally after any swapchain
    // recreation.
    ImGui_ImplVulkan_SetMinImageCount(imageCount);
}

