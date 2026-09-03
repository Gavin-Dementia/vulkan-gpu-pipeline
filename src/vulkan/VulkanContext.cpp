#include "vulkan/VulkanContext.h"
#include "vulkan/resource/ObjLoader.h"
#include "vulkan/pipeline/VulkanEnvCapturePipeline.h"
#include "vulkan/pipeline/VulkanIrradiancePipeline.h"
#include "vulkan/pipeline/VulkanPrefilterPipeline.h"
#include "vulkan/pipeline/VulkanBRDFLutPipeline.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

void VulkanContext::init(GLFWwindow* window)
{
    window_ = window;

    initCore();
    initSceneData();
    initCullingResources();

    imguiLayer_.init(
        window_,
        instance_.get(),
        device_.getPhysical(),
        device_.get(),
        device_.getGraphicsQueueFamily(),
        device_.getGraphicsQueue(),
        renderPass_.get(),
        swapchain_.getImageViews().size()
    );

    std::cout << "Vulkan Context initialized\n";
}

// =========================================================
// Core Vulkan objects: Instance -> Device -> Swapchain ->
// DepthBuffer -> RenderPass -> Pipeline -> Framebuffer
// =========================================================
void VulkanContext::initCore()
{
    instance_.create();
    surface_.create(instance_.get(), window_);

    device_.create(instance_.get(), surface_.get());

    swapchain_.create(
        device_.getPhysical(),
        device_.get(),
        surface_.get(),
        window_
    );

    commandPool_.create(
        device_.get(),
        device_.getGraphicsQueueFamily()
    );

    depthBuffer_.create(
        device_.getPhysical(),
        device_.get(),
        swapchain_.getExtent()
    );

    renderPass_.create(
        device_.get(),
        swapchain_.getImageFormat(),
        depthBuffer_.format()
    );

    uniformBuffer_.create(device_.getPhysical(), device_.get());

    // Real PBR material as of Phase 20 (see docs/TECHNICAL_NOTES.md §42) -
    // ambientCG's CC0 "Bricks097" (README's Asset Credits has the full
    // attribution): test_texture.png/normal.png/ao.png are that material's
    // real photogrammetry Color/NormalGL/AmbientOcclusion maps, resized to
    // 512x512. metallic_roughness.png is synthesized from its Roughness
    // map (G channel) with metallic (B channel) held at a constant 0 -
    // physically correct for a non-metal material, not a fabricated value
    // - following glTF's G=roughness/B=metallic channel convention.
    // Replaced Phase 8 milestone 2's self-generated flat/gradient
    // placeholders (a flat "no perturbation" normal map, a flat near-white
    // AO map) entirely.
    material_.load(
        device_.getPhysical(), device_.get(),
        commandPool_.get(), device_.getGraphicsQueue(),
        "assets/test_texture.png",
        "assets/normal.png",
        "assets/metallic_roughness.png",
        "assets/ao.png"
    );

    // Shared per-frame scene/light data (SceneData.h) - one buffer bound
    // as binding 2 on every material's descriptor set, not duplicated
    // per object, since it's identical for every draw in a given frame.
    sceneDataBuffer_.create(
        device_.getPhysical(), device_.get(), sizeof(SceneData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Shadow map created here (before both descriptor sets below, which
    // both bind it at binding 3) even though its render pass/framebuffer/
    // pipeline are set up later in this function - only the image/view/
    // sampler need to exist yet.
    shadowMap_.create(device_.getPhysical(), device_.get());

    descriptor_.create(
        device_.get(),
        uniformBuffer_.get(),
        material_,
        sceneDataBuffer_.get(),
        shadowMap_.view(),
        shadowMap_.sampler()
    );

    // Projectile gets its own UBO + descriptor set (same shared texture) -
    // it needs a different model matrix (identity, no spin) than the grid
    // in the same frame, and a single mutable UBO can't hold two different
    // values for two draw calls recorded in the same command buffer (the
    // GPU reads whichever value is in memory at execute time, not at
    // record time - see TECHNICAL_NOTES.md for the full rationale).
    projectileUniformBuffer_.create(device_.getPhysical(), device_.get());
    projectileDescriptor_.create(
        device_.get(),
        projectileUniformBuffer_.get(),
        material_,
        sceneDataBuffer_.get(),
        shadowMap_.view(),
        shadowMap_.sampler()
    );

    // Offscreen scene target: GeometryPass/LightingPass/PostProcess
    // (PassStage::Graphics) render here instead of directly to the
    // swapchain, so ImGui can display the result inside a dockable
    // "Viewport" panel alongside the debug windows instead of everything
    // overlapping the same fullscreen image. 1280x720 is just the
    // startup default (matching the fixed GLFW window size) - resizable
    // at runtime as of docs/TECHNICAL_NOTES.md §36, see
    // VulkanContext::resizeSceneTarget().
    sceneColorTarget_.create(device_.getPhysical(), device_.get(), 1280, 720);
    sceneColorDepth_.create(
        device_.getPhysical(), device_.get(), sceneColorTarget_.extent()
    );
    sceneRenderPass_.createOffscreenColor(
        device_.get(), VulkanSceneColorTarget::FORMAT, sceneColorDepth_.format()
    );
    sceneFramebuffer_.create(
        device_.get(),
        sceneRenderPass_.get(),
        { sceneColorTarget_.view() },
        sceneColorDepth_.view(),
        sceneColorTarget_.extent()
    );

    // IBL (see docs/TECHNICAL_NOTES.md §33/§34/§35): must run here, not
    // as a separate step after initCore() returns - it needs
    // sceneRenderPass_/sceneColorTarget_ (just created above) for
    // skyboxPipeline_, and pipeline_.create() right below now needs
    // iblDescriptor_'s layout as its second descriptor set, so the whole
    // bake has to complete before that call.
    initEnvironment();

    // Geometry pipeline now targets the offscreen scene render pass, not
    // the swapchain's - the swapchain's renderPass_/framebuffer_ below are
    // used only to host the UI-stage pass (see FrameGraph::PassStage::UI).
    // Set 0 = descriptor_ (per-object material data), set 1 =
    // iblDescriptor_ (ambient-lighting data, shared globally).
    pipeline_.create(
        device_.get(),
        sceneColorTarget_.extent(),
        sceneRenderPass_.get(),
        descriptor_.layout(),
        iblDescriptor_.layout()
    );

    framebuffer_.create(
        device_.get(),
        renderPass_.get(),
        swapchain_.getImageViews(),
        depthBuffer_.view(),
        swapchain_.getExtent()
    );

    // Shadow map's image/view/sampler were already created above (before
    // descriptor_/projectileDescriptor_, which both bind it at binding 3);
    // its render pass/framebuffer/pipeline are independent of that and are
    // created here alongside the main pipeline/framebuffer.
    shadowRenderPass_.createDepthOnly(device_.get(), VulkanShadowMap::FORMAT);

    shadowFramebuffer_.createDepthOnly(
        device_.get(),
        shadowRenderPass_.get(),
        shadowMap_.view(),
        shadowMap_.extent()
    );

    shadowPipeline_.create(
        device_.get(),
        shadowMap_.extent(),
        shadowRenderPass_.get()
    );
}

// =========================================================
// Live-resized viewport target (see docs/TECHNICAL_NOTES.md §36).
// Recreates sceneColorTarget_/sceneColorDepth_/sceneFramebuffer_ and the
// two pipelines whose VkViewport/scissor is baked in at creation time
// (pipeline_, skyboxPipeline_) at a new size. sceneRenderPass_ is left
// untouched - a VkRenderPass encodes attachment format/structure only,
// not extent (the same fact IBL's bake reused one render pass across
// several differently-sized framebuffers already relied on). Called by
// FrameRenderer at the top of drawFrame(), never mid-frame - see that
// call site for why.
// =========================================================
void VulkanContext::resizeSceneTarget(uint32_t width, uint32_t height)
{
    // Single authoritative clamp - callers (FrameRenderer) don't need to
    // pre-clamp whatever ImGui::GetContentRegionAvail() reports, which
    // can transiently be 0 while a dock panel is being torn down/rebuilt.
    constexpr uint32_t kMinDimension = 64;
    width  = std::max(width, kMinDimension);
    height = std::max(height, kMinDimension);

    if (width == sceneColorTarget_.extent().width && height == sceneColorTarget_.extent().height)
        return;   // no-op - avoids a needless vkDeviceWaitIdle stall

    // sceneColorTarget_/sceneColorDepth_/pipeline_/skyboxPipeline_ are
    // single, shared instances (not duplicated per frame-in-flight), so
    // a per-slot fence wait isn't enough to know the GPU is done with
    // them - only every in-flight frame across both slots being fully
    // retired is provably sufficient. A full device idle is a real stall
    // (visible as a brief hitch while dragging the Viewport panel's
    // border), an accepted tradeoff for keeping this resize path simple.
    vkDeviceWaitIdle(device_.get());

    sceneFramebuffer_.destroy(device_.get());
    pipeline_.destroy(device_.get());
    skyboxPipeline_.destroy(device_.get());
    sceneColorDepth_.destroy(device_.get());
    sceneColorTarget_.destroy(device_.get());

    sceneColorTarget_.create(device_.getPhysical(), device_.get(), width, height);
    sceneColorDepth_.create(
        device_.getPhysical(), device_.get(), sceneColorTarget_.extent()
    );
    sceneFramebuffer_.create(
        device_.get(),
        sceneRenderPass_.get(),
        { sceneColorTarget_.view() },
        sceneColorDepth_.view(),
        sceneColorTarget_.extent()
    );
    pipeline_.create(
        device_.get(),
        sceneColorTarget_.extent(),
        sceneRenderPass_.get(),
        descriptor_.layout(),
        iblDescriptor_.layout()
    );
    skyboxPipeline_.create(
        device_.get(), sceneColorTarget_.extent(), sceneRenderPass_.get(), skyboxDescriptor_.layout()
    );
}

// =========================================================
// Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39).
// Recreates framebuffer_/depthBuffer_/swapchain_ at the window's current
// framebuffer size. renderPass_ is left untouched - same "format/
// structure only, no extent" fact resizeSceneTarget() above already
// relies on for sceneRenderPass_. pipeline_/skyboxPipeline_ need no
// recreation here (unlike resizeSceneTarget()'s), since neither targets
// the swapchain - both bake a VkViewport against sceneColorTarget_'s
// extent instead, which this resize doesn't touch.
// =========================================================
void VulkanContext::resizeSwapchain()
{
    // Shared across both frames-in-flight, not per-slot, so only a full
    // device idle is provably sufficient before destroying them - same
    // stall/simplicity tradeoff resizeSceneTarget() already accepts.
    vkDeviceWaitIdle(device_.get());

    framebuffer_.destroy(device_.get());
    depthBuffer_.destroy(device_.get());
    swapchain_.destroy(device_.get());

    swapchain_.create(
        device_.getPhysical(),
        device_.get(),
        surface_.get(),
        window_
    );

    depthBuffer_.create(
        device_.getPhysical(),
        device_.get(),
        swapchain_.getExtent()
    );

    framebuffer_.create(
        device_.get(),
        renderPass_.get(),
        swapchain_.getImageViews(),
        depthBuffer_.view(),
        swapchain_.getExtent()
    );
}

// =========================================================
// IBL Milestones 1-3 (see docs/TECHNICAL_NOTES.md §33/§34/§35):
// environment cubemap + procedural sky bake + skybox pipeline (M1), a
// diffuse irradiance cubemap convolved from the environment (M2), and a
// GGX-importance-sampled specular prefilter + BRDF integration LUT (M3,
// Karis's split-sum specular IBL approximation). All four bakes run once
// at startup, not per-frame - they're pure functions of lightDirection_
// at this point in time, so a later live change to the light direction
// (the "Lighting" ImGui window) will not move the sun in the skybox or
// update the ambient term, an accepted limitation carried over from M1.
// =========================================================
void VulkanContext::initEnvironment()
{
    constexpr uint32_t kEnvFaceSize = 512;
    constexpr uint32_t kIrradianceFaceSize = 32;   // diffuse irradiance is
        // extremely low-frequency after cosine-weighted convolution - 32
        // per face is the standard, well-tested value, not a compromise.
    constexpr VkFormat kEnvFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    environmentCubemap_.create(
        device_.getPhysical(), device_.get(), kEnvFaceSize, kEnvFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    );

    // Created early (host-side only - the actual image content isn't
    // ready until the bake below completes) so the irradiance-convolution
    // draws further down, in the same command buffer, can bind it as
    // their input. Also the same instance the live skybox draw uses -
    // see CubeSamplerDescriptor's class comment.
    skyboxDescriptor_.create(
        device_.get(), environmentCubemap_.cubeView(), environmentCubemap_.sampler()
    );

    // Locally-scoped bake resources - created, used, destroyed within
    // this function; only environmentCubemap_/irradianceCubemap_/
    // prefilteredCubemap_/brdfLut_ (and the persistent
    // skyboxDescriptor_/iblDescriptor_/skyboxPipeline_ created at the
    // end) survive it. One render pass, reused for three bakes - a
    // VkRenderPass encodes attachment format/structure only, not extent,
    // so the same color-only/no-depth shape backs the 512^2 environment
    // bake, the 32^2 irradiance bake, and the 128^2..8^2 prefilter mip
    // chain below (via several different framebuffers; the BRDF LUT
    // bake needs its own render pass, different format).
    VulkanRenderPass captureRenderPass;
    captureRenderPass.createColorOnly(device_.get(), kEnvFormat);

    std::vector<VkImageView> faceViews;
    for (int i = 0; i < 6; i++)
        faceViews.push_back(environmentCubemap_.faceView(i));

    VkExtent2D faceExtent{ kEnvFaceSize, kEnvFaceSize };

    VulkanFramebuffer captureFramebuffer;
    captureFramebuffer.createColorOnly(device_.get(), captureRenderPass.get(), faceViews, faceExtent);

    VulkanEnvCapturePipeline capturePipeline;
    capturePipeline.create(device_.get(), faceExtent, captureRenderPass.get());

    // Standard cubemap capture table (LearnOpenGL's IBL tutorial / Sascha
    // Willems' Vulkan generateCubemaps() convention, not hand-derived) -
    // face order matches Vulkan's cube array-layer convention:
    // +X,-X,+Y,-Y,+Z,-Z.
    static const glm::vec3 kCaptureForward[6] = {
        { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
    };
    static const glm::vec3 kCaptureUp[6] = {
        { 0,-1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}, { 0,-1, 0}, { 0,-1, 0}
    };

    glm::mat4 captureProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    captureProj[1][1] *= -1;   // same Y-flip Camera::getProjectionMatrix() applies

    glm::vec3 sunDir = glm::normalize(-lightDirection_);
    float sunCosHalfAngle = std::cos(glm::radians(2.0f));   // ~4-degree-wide sun disk

    // One-shot command buffer - exact pattern VulkanTexture::
    // transitionLayout()/copyBufferToImage() already use for setup-time
    // GPU work (src/vulkan/texture/VulkanTexture.cpp).
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_.get();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_.get(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    for (int face = 0; face < 6; face++)
    {
        glm::mat4 captureView = glm::lookAt(glm::vec3(0.0f), kCaptureForward[face], kCaptureUp[face]);

        SkyCapturePushConstants pc{};
        pc.invViewProj  = glm::inverse(captureProj * captureView);
        pc.cameraPos    = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        pc.sunDirAndCos = glm::vec4(sunDir, sunCosHalfAngle);

        VkClearValue clearValue{};
        clearValue.color = { 0.0f, 0.0f, 0.0f, 1.0f };

        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = captureRenderPass.get();
        rp.framebuffer = captureFramebuffer.get()[face];
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = faceExtent;
        rp.clearValueCount = 1;
        rp.pClearValues = &clearValue;

        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, capturePipeline.get());
        vkCmdPushConstants(
            cmd, capturePipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SkyCapturePushConstants), &pc
        );
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
    }

    // Orders the memory access after the 6 render passes' finalLayout
    // transitions - each face's transition is already per-subresource-
    // correct (Vulkan tracks layout per mip/layer, not per view), this
    // barrier just makes the write->read visibility explicit, matching
    // FrameRenderer.cpp's shadowBarrier/sceneBarrier precedent
    // (oldLayout==newLayout, ordering only) - AND is load-bearing here:
    // the irradiance-convolution draws right below, in this same command
    // buffer, sample environmentCubemap_, so this barrier is what makes
    // that safe (a mid-command-buffer pipeline barrier orders everything
    // before it against everything after it in submission order - no
    // second submit/wait needed).
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = environmentCubemap_.image();
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 6;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    // --- IBL Milestone 2: diffuse irradiance convolution, same command
    // buffer, right after the environment bake it reads from. ---
    irradianceCubemap_.create(
        device_.getPhysical(), device_.get(), kIrradianceFaceSize, kEnvFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    );

    std::vector<VkImageView> irrFaceViews;
    for (int i = 0; i < 6; i++)
        irrFaceViews.push_back(irradianceCubemap_.faceView(i));

    VkExtent2D irrExtent{ kIrradianceFaceSize, kIrradianceFaceSize };

    VulkanFramebuffer irradianceFramebuffer;
    irradianceFramebuffer.createColorOnly(device_.get(), captureRenderPass.get(), irrFaceViews, irrExtent);

    // Reuses skyboxDescriptor_'s layout (both are just "one cube sampler
    // at binding 0") and the same kCaptureForward/kCaptureUp/captureProj
    // capture-direction table the environment bake already established -
    // baking a cubemap face is baking a cubemap face, regardless of what
    // the fragment shader does with the sampled direction.
    VulkanIrradiancePipeline irradiancePipeline;
    irradiancePipeline.create(device_.get(), irrExtent, captureRenderPass.get(), skyboxDescriptor_.layout());

    for (int face = 0; face < 6; face++)
    {
        glm::mat4 captureView = glm::lookAt(glm::vec3(0.0f), kCaptureForward[face], kCaptureUp[face]);

        SkyboxPushConstants pc{};
        pc.invViewProj = glm::inverse(captureProj * captureView);
        pc.cameraPos   = glm::vec4(0.0f);

        VkClearValue clearValue{};
        clearValue.color = { 0.0f, 0.0f, 0.0f, 1.0f };

        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = captureRenderPass.get();
        rp.framebuffer = irradianceFramebuffer.get()[face];
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = irrExtent;
        rp.clearValueCount = 1;
        rp.pClearValues = &clearValue;

        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, irradiancePipeline.get());
        VkDescriptorSet envDs = skyboxDescriptor_.set();
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, irradiancePipeline.layout(),
            0, 1, &envDs, 0, nullptr
        );
        vkCmdPushConstants(
            cmd, irradiancePipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SkyboxPushConstants), &pc
        );
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
    }

    VkImageMemoryBarrier irrBarrier = barrier;
    irrBarrier.image = irradianceCubemap_.image();
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &irrBarrier
    );

    // --- IBL Milestone 3: specular prefilter, same command buffer,
    // right after the irradiance bake it doesn't depend on but shares
    // captureRenderPass/skyboxDescriptor_/the capture-direction table
    // with. 5 mips (roughness 0.0/0.25/0.5/0.75/1.0), one
    // VulkanPrefilterPipeline per mip since every pipeline class in this
    // codebase bakes a static viewport at creation time (no dynamic-
    // viewport-state precedent exists to reuse instead). ---
    constexpr uint32_t kPrefilterMipLevels    = 5;
    constexpr uint32_t kPrefilterBaseFaceSize = 128;

    prefilteredCubemap_.create(
        device_.getPhysical(), device_.get(), kPrefilterBaseFaceSize, kEnvFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        kPrefilterMipLevels
    );

    std::vector<VulkanFramebuffer> prefilterFramebuffers(kPrefilterMipLevels);
    std::vector<VulkanPrefilterPipeline> prefilterPipelines(kPrefilterMipLevels);

    for (uint32_t mip = 0; mip < kPrefilterMipLevels; mip++)
    {
        uint32_t mipFaceSize = kPrefilterBaseFaceSize >> mip;
        VkExtent2D mipExtent{ mipFaceSize, mipFaceSize };

        std::vector<VkImageView> mipFaceViews;
        for (int face = 0; face < 6; face++)
            mipFaceViews.push_back(prefilteredCubemap_.faceView(face, mip));

        prefilterFramebuffers[mip].createColorOnly(device_.get(), captureRenderPass.get(), mipFaceViews, mipExtent);
        prefilterPipelines[mip].create(device_.get(), mipExtent, captureRenderPass.get(), skyboxDescriptor_.layout());

        float roughness = float(mip) / float(kPrefilterMipLevels - 1);

        for (int face = 0; face < 6; face++)
        {
            glm::mat4 captureView = glm::lookAt(glm::vec3(0.0f), kCaptureForward[face], kCaptureUp[face]);

            PrefilterPushConstants pc{};
            pc.invViewProj = glm::inverse(captureProj * captureView);
            pc.cameraPos   = glm::vec4(0.0f);
            pc.roughness   = roughness;

            VkClearValue clearValue{};
            clearValue.color = { 0.0f, 0.0f, 0.0f, 1.0f };

            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = captureRenderPass.get();
            rp.framebuffer = prefilterFramebuffers[mip].get()[face];
            rp.renderArea.offset = {0, 0};
            rp.renderArea.extent = mipExtent;
            rp.clearValueCount = 1;
            rp.pClearValues = &clearValue;

            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prefilterPipelines[mip].get());
            VkDescriptorSet prefilterEnvDs = skyboxDescriptor_.set();
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prefilterPipelines[mip].layout(),
                0, 1, &prefilterEnvDs, 0, nullptr
            );
            vkCmdPushConstants(
                cmd, prefilterPipelines[mip].layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(PrefilterPushConstants), &pc
            );
            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRenderPass(cmd);
        }
    }

    VkImageMemoryBarrier prefilterBarrier = barrier;
    prefilterBarrier.image                       = prefilteredCubemap_.image();
    prefilterBarrier.subresourceRange.levelCount  = kPrefilterMipLevels;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &prefilterBarrier
    );

    // --- IBL Milestone 3: BRDF integration LUT, same command buffer.
    // Zero dependency on anything else baked here (a pure function of
    // UV, no texture input) - ordered last purely for readability.
    // Different format than the HDR cubemaps, so it needs its own
    // VulkanRenderPass (can't reuse captureRenderPass). ---
    constexpr uint32_t kBrdfLutSize = 512;
    constexpr VkFormat kBrdfLutFormat = VK_FORMAT_R16G16_SFLOAT;

    brdfLut_.create(
        device_.getPhysical(), device_.get(), kBrdfLutSize, kBrdfLutFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    );

    VulkanRenderPass brdfRenderPass;
    brdfRenderPass.createColorOnly(device_.get(), kBrdfLutFormat);

    VkExtent2D brdfExtent{ kBrdfLutSize, kBrdfLutSize };
    VulkanFramebuffer brdfFramebuffer;
    brdfFramebuffer.createColorOnly(device_.get(), brdfRenderPass.get(), { brdfLut_.view() }, brdfExtent);

    VulkanBRDFLutPipeline brdfPipeline;
    brdfPipeline.create(device_.get(), brdfExtent, brdfRenderPass.get());

    VkClearValue brdfClear{};
    brdfClear.color = { 0.0f, 0.0f, 0.0f, 1.0f };

    VkRenderPassBeginInfo brdfRp{};
    brdfRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    brdfRp.renderPass = brdfRenderPass.get();
    brdfRp.framebuffer = brdfFramebuffer.get()[0];
    brdfRp.renderArea.offset = {0, 0};
    brdfRp.renderArea.extent = brdfExtent;
    brdfRp.clearValueCount = 1;
    brdfRp.pClearValues = &brdfClear;

    vkCmdBeginRenderPass(cmd, &brdfRp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, brdfPipeline.get());
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    VkImageMemoryBarrier brdfBarrier{};
    brdfBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    brdfBarrier.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    brdfBarrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    brdfBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    brdfBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    brdfBarrier.image               = brdfLut_.image();
    brdfBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    brdfBarrier.subresourceRange.baseMipLevel   = 0;
    brdfBarrier.subresourceRange.levelCount     = 1;
    brdfBarrier.subresourceRange.baseArrayLayer = 0;
    brdfBarrier.subresourceRange.layerCount     = 1;
    brdfBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    brdfBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &brdfBarrier
    );

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    vkQueueSubmit(device_.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_.getGraphicsQueue());

    vkFreeCommandBuffers(device_.get(), commandPool_.get(), 1, &cmd);

    irradiancePipeline.destroy(device_.get());
    irradianceFramebuffer.destroy(device_.get());
    for (uint32_t mip = 0; mip < kPrefilterMipLevels; mip++)
    {
        prefilterPipelines[mip].destroy(device_.get());
        prefilterFramebuffers[mip].destroy(device_.get());
    }
    brdfPipeline.destroy(device_.get());
    brdfFramebuffer.destroy(device_.get());
    brdfRenderPass.destroy(device_.get());
    capturePipeline.destroy(device_.get());
    captureFramebuffer.destroy(device_.get());
    captureRenderPass.destroy(device_.get());

    // Bundles all three ambient-lighting textures (irradiance + M2,
    // prefiltered specular + BRDF LUT + M3) into one 3-binding set,
    // bound as descriptor set 1 on the main graphics pipeline.
    iblDescriptor_.create(
        device_.get(),
        irradianceCubemap_.cubeView(), irradianceCubemap_.sampler(),
        prefilteredCubemap_.cubeView(), prefilteredCubemap_.sampler(),
        brdfLut_.view(), brdfLut_.sampler()
    );

    // Persistent skybox pipeline - sampled every frame by GeometryPass's
    // skybox draw (FrameRenderer.cpp). skyboxDescriptor_ itself was
    // already created above, before the bake.
    skyboxPipeline_.create(
        device_.get(), sceneColorTarget_.extent(), sceneRenderPass_.get(), skyboxDescriptor_.layout()
    );

    std::cout << "[VulkanContext] IBL: environment + diffuse irradiance + specular prefilter + BRDF LUT baked, skybox pipeline ready\n";
}

// =========================================================
// Scene data: mesh (Vertex/Index Buffer) + instance grid
// =========================================================
void VulkanContext::initSceneData()
{
    // LOD0 uses a UV/normal-mapped re-export of the same base mesh (Phase 8
    // milestone 2, see docs/TECHNICAL_NOTES.md) so it can actually sample
    // the new texture set; LOD1/LOD2 keep the original UV-less meshes -
    // matching UV-preserving decimations would need a 3D tool this
    // environment doesn't have, so they stay a known, flagged gap rather
    // than blocking this milestone.
    const std::array<std::string, 3> lodPaths = {
        "assets/suzanne_pbr.obj",
        "assets/suzanne_lod1.obj",
        "assets/suzanne_lod2.obj"
    };

    for (int i = 0; i < 3; i++)
    {
        auto mesh = ObjLoader::load(lodPaths[i]);

        lods_[i].vertexBuffer.create(
            device_.getPhysical(), device_.get(),
            commandPool_.get(), device_.getGraphicsQueue(),
            mesh.vertices
        );

        lods_[i].indexBuffer.create(
            device_.getPhysical(), device_.get(),
            commandPool_.get(), device_.getGraphicsQueue(),
            mesh.indices
        );

        // LOD0 is the most detailed variant, so its bounds are the most
        // representative object radius for the (LOD-independent) culling
        // test - LOD1/2 are decimated versions of the same shape and are
        // never larger than LOD0.
        if (i == 0)
        {
            boundingSphereRadius_ = mesh.boundingRadius;
            // Collision volume starts equal to the render/culling bounds
            // (a sensible, mesh-derived default) but is a separate value -
            // see the accessor comment in VulkanContext.h.
            collisionRadius_ = boundingSphereRadius_;
        }

        // Mesh-detail-derived LOD2 threshold (see docs/TECHNICAL_NOTES.md
        // §40) - captured here, right after each LOD's own load, since
        // mesh.indices goes out of scope at the end of this loop iteration.
        uint32_t triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3);
        if (i == 0)      lod0TriangleCount_ = triangleCount;
        else if (i == 1) lod1TriangleCount_ = triangleCount;
        else             lod2TriangleCount_ = triangleCount;
    }

    // Mesh-detail-derived LOD2 threshold default (§40): lod1ScreenSize_
    // stays the one manually-anchored top-level threshold (no "LOD -1"
    // mesh exists to derive it from), but lod2ScreenSize_'s startup value
    // is now grounded in how much simpler LOD2 actually is than LOD1,
    // instead of an independent hand-picked constant. Overwrites the
    // placeholder literal from the member declaration.
    lod2ScreenSize_ = lod1ScreenSize_ * lod2DetailRatio();

    // 7x7x7 grid, spacing 3.0, centered on origin
    std::vector<InstanceData> instances(OBJECT_COUNT);

    float spacing = 3.0f;
    float halfGrid = (GRID_SIZE - 1) * spacing * 0.5f;

    uint32_t idx = 0;
    for (uint32_t x = 0; x < GRID_SIZE; x++)
    for (uint32_t y = 0; y < GRID_SIZE; y++)
    for (uint32_t z = 0; z < GRID_SIZE; z++)
    {
        glm::vec3 pos = {
            x * spacing - halfGrid,
            y * spacing - halfGrid,
            z * spacing - halfGrid
        };
        instances[idx++].position = glm::vec4(pos, 1.0f);
    }

    // Cache instance world positions for object buffer setup
    cachedInstances_ = std::move(instances);

    // Single-entry instance buffer for the mouse-fired projectile - the
    // graphics pipeline's vertex input layout always expects something
    // bound at binding 1 (per-instance position), even for a non-instanced
    // draw of one object. No STORAGE_BUFFER_BIT needed since nothing
    // writes it via compute - it's just a CPU-updated translation.
    projectileInstanceBuffer_.create(
        device_.getPhysical(), device_.get(), sizeof(InstanceData),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
}

// =========================================================
// GPU culling resources: ObjectBuffer, VisibleInstanceBuffer,
// IndirectDrawBuffer, FrustumBuffer, ComputeDescriptor/Pipeline
// =========================================================
void VulkanContext::initCullingResources()
{
    struct ComputeObjectData { glm::vec4 boundingSphere; };  // xyz=center, w=radius

    std::vector<ComputeObjectData> objects(OBJECT_COUNT);

    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
    {
        objects[i].boundingSphere = glm::vec4(
            glm::vec3(cachedInstances_[i].position),
            boundingSphereRadius_
        );
    }

    VkDeviceSize objSize = sizeof(ComputeObjectData) * OBJECT_COUNT;

    // VERTEX_BUFFER_BIT alongside STORAGE_BUFFER_BIT: the shadow pass
    // (FrameRenderer.cpp) binds this same buffer directly as its
    // per-instance vertex input, reusing culling.comp's per-frame
    // bounding-sphere data instead of a separate buffer - see
    // architecture.md's shadow mapping notes.
    objectBuffer_.create(
        device_.getPhysical(), device_.get(), objSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    objectBuffer_.upload(device_.get(), objects.data(), objSize);

    // Output buffer sized for worst case: all instances visible
    VkDeviceSize visibleInstanceSize = sizeof(InstanceData) * OBJECT_COUNT;
    VkDeviceSize indirectDrawSize    = sizeof(DrawIndirectCommand);

    for (int i = 0; i < 3; i++)
    {
        lods_[i].visibleInstanceBuffer.create(
            device_.getPhysical(), device_.get(), visibleInstanceSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        // 每个LOD的indexCount不同
        uint32_t lodIndexCount = lods_[i].indexBuffer.indexCount();
        lods_[i].indirectDrawBuffer.create(
            device_.getPhysical(), device_.get(), lodIndexCount
        );
    }

    // Frustum buffer: 6 planes + cameraPos + lodParams = 8 vec4 = 128 bytes
    VkDeviceSize frustumSize = sizeof(FrustumPlanes);

    frustumBuffer_.create(
        device_.getPhysical(), device_.get(), frustumSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Shadow pass's light-frustum-culled instance set - same shapes as a
    // single LOD's {visibleInstanceBuffer, indirectDrawBuffer} above, plus
    // its own frustum UBO (only planes[6] is used - camera pos/lodParams
    // are meaningless for the light, but reusing FrustumPlanes keeps the
    // std140 layout identical to the camera frustum's, no separate GLSL
    // struct needed). Sized for worst case (all OBJECT_COUNT visible),
    // indexed by LOD0's mesh - the shadow pass always draws LOD0 geometry
    // regardless of camera distance.
    shadowVisibleInstanceBuffer_.create(
        device_.getPhysical(), device_.get(), visibleInstanceSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    shadowIndirectDrawBuffer_.create(
        device_.getPhysical(), device_.get(), lods_[0].indexBuffer.indexCount()
    );
    lightFrustumBuffer_.create(
        device_.getPhysical(), device_.get(), frustumSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Hierarchical culling (coarse pass) - 64 cluster bounding spheres,
    // re-uploaded every frame from the CPU (see updateInstanceSimulation()),
    // plus the coarse shader's two output flag buffers. HOST_VISIBLE like
    // every other compute SSBO in this codebase, not DEVICE_LOCAL - the
    // two flag buffers are also read back on the CPU every frame for the
    // "GPU Culling Stats" ImGui counts, same safe post-fence-wait readback
    // FrameRenderer.cpp already uses for the LOD visible counts.
    VkDeviceSize clusterBufferSize  = sizeof(glm::vec4) * CLUSTER_COUNT;
    VkDeviceSize clusterVisibleSize = sizeof(uint32_t) * CLUSTER_COUNT;

    clusterBuffer_.create(
        device_.getPhysical(), device_.get(), clusterBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    clusterVisibleCameraBuffer_.create(
        device_.getPhysical(), device_.get(), clusterVisibleSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    clusterVisibleLightBuffer_.create(
        device_.getPhysical(), device_.get(), clusterVisibleSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    std::array<VkBuffer, 3> visibleBufs = {
        lods_[0].visibleInstanceBuffer.get(),
        lods_[1].visibleInstanceBuffer.get(),
        lods_[2].visibleInstanceBuffer.get()
    };

    std::array<VkBuffer, 3> indirectBufs = {
        lods_[0].indirectDrawBuffer.get(),
        lods_[1].indirectDrawBuffer.get(),
        lods_[2].indirectDrawBuffer.get()
    };

    computeDescriptor_.create(
        device_.get(),
        objectBuffer_.get(),
        visibleBufs, indirectBufs,
        frustumBuffer_.get(),
        shadowVisibleInstanceBuffer_.get(),
        shadowIndirectDrawBuffer_.get(),
        lightFrustumBuffer_.get(),
        clusterBuffer_.get(),
        clusterVisibleCameraBuffer_.get(),
        clusterVisibleLightBuffer_.get(),
        objSize, visibleInstanceSize,
        indirectDrawSize, frustumSize,
        clusterBufferSize, clusterVisibleSize
    );

    computePipeline_.create(
        device_.get(), computeDescriptor_.layout(),
        "shaders/compiled/culling.comp.spv");
    computePipelineCoarse_.create(
        device_.get(), computeDescriptor_.layout(),
        "shaders/compiled/cullingCoarse.comp.spv");

    // cachedInstances_ is kept alive as the permanent rest formation
    // (used by resetInstanceFormation()) instead of being freed here.
    instanceCurrentPositions_.resize(OBJECT_COUNT);
    instanceVelocities_.assign(OBJECT_COUNT, glm::vec3(0.0f));
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        instanceCurrentPositions_[i] = glm::vec3(cachedInstances_[i].position);
}

// =========================================================
// Grid collision + scatter (Phase 7 milestone 2)
// =========================================================
void VulkanContext::updateInstanceSimulation(float deltaTime)
{
    constexpr float kDampingPerSecond = 0.05f;   // fraction of velocity retained after 1 full second
    constexpr float kProjectileRadius = 0.3f;
    constexpr float kBlastRadius      = 6.0f;
    constexpr float kImpulseStrength  = 8.0f;

    float dampingFactor = std::pow(kDampingPerSecond, deltaTime);   // framerate-independent decay

    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
    {
        instanceCurrentPositions_[i] += instanceVelocities_[i] * deltaTime;
        instanceVelocities_[i] *= dampingFactor;
    }

    if (projectile_.isActive())
    {
        // Swept (segment-vs-sphere), not a single end-of-frame point check
        // (see docs/TECHNICAL_NOTES.md §41) - tests the whole path the
        // projectile moved this frame, [previousPosition(), position()],
        // against every instance's collision sphere, instead of only where
        // it ended up. A large enough deltaTime/speed could otherwise let
        // it move further in one frame than the hit radius, skipping clean
        // over an instance without the old point check ever registering
        // inside it.
        glm::vec3 segStart = projectile_.previousPosition();
        glm::vec3 segEnd   = projectile_.position();
        glm::vec3 segDir   = segEnd - segStart;
        float segLenSq = glm::dot(segDir, segDir);
        float hitDist = collisionRadius_ + kProjectileRadius;   // collision volume, not the render/culling radius

        // Find the *earliest* hit along the segment, not just the first
        // instance index that happens to overlap it - with a long enough
        // sweep, more than one instance can be within hitDist of the path,
        // and the projectile should stop at whichever it actually reaches
        // first, not an arbitrary later one.
        bool  hasHit = false;
        float bestT  = 2.0f;   // sentinel above the [0,1] clamp range below

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            glm::vec3 toInstance = instanceCurrentPositions_[i] - segStart;
            float t = segLenSq > 1e-8f
                ? glm::clamp(glm::dot(toInstance, segDir) / segLenSq, 0.0f, 1.0f)
                : 0.0f;
            glm::vec3 closest = segStart + segDir * t;
            if (glm::length(instanceCurrentPositions_[i] - closest) < hitDist && t < bestT)
            {
                hasHit = true;
                bestT  = t;
            }
        }

        if (hasHit)
        {
            // Blast: radial push falling off with distance from the actual
            // impact point along the sweep (not wherever the projectile
            // ended up this frame, which can be well past it for a fast-
            // moving/large-deltaTime frame), applied to every instance
            // within range - not just the one instance that was touched.
            glm::vec3 impactPoint = segStart + segDir * bestT;
            for (uint32_t j = 0; j < OBJECT_COUNT; j++)
            {
                glm::vec3 offset = instanceCurrentPositions_[j] - impactPoint;
                float dist = glm::length(offset);
                if (dist < kBlastRadius)
                {
                    glm::vec3 dir = (dist > 0.001f) ? (offset / dist) : glm::vec3(0.0f, 1.0f, 0.0f);
                    float falloff = 1.0f - (dist / kBlastRadius);
                    instanceVelocities_[j] += dir * kImpulseStrength * falloff;
                }
            }
            projectile_.stop();
        }
    }

    // Mutual collision: resolve overlaps between instances themselves,
    // not just projectile-vs-instance. Without this, scattered instances
    // that drift close to each other (or to still-resting neighbors -
    // grid spacing is only 3.0 against a ~1.49 bounding radius, so
    // resting instances already sit just 0.03 units apart at closest)
    // visibly clip through each other once their blast velocity settles.
    //
    // Hybrid response (§30): a velocity impulse (equal-mass, restitution-
    // scaled, along the contact normal) handles the visible "bounce apart"
    // motion, applied only while a pair is actually approaching
    // (velAlongNormal < 0) - a separating or already-resting pair gets no
    // impulse, since an impulse can't push apart two objects with ~zero
    // relative velocity. That's exactly the case the positional-pushout
    // step below still exists for: it's now a much lighter safety net
    // (10% of the gap per side, not the pre-impulse 30%) whose only job is
    // guaranteeing eventual separation for resting overlap, not doing the
    // primary separating - the impulse does that dynamically now.
    // Deliberately tuned loose either way, not a strict non-overlap
    // constraint: minSeparation (1.5x radius, not the geometrically "just
    // touching" 2x) tolerates some visual overlap for a denser scatter
    // look. Both steps run every frame (not just on impact) in the same
    // O(n^2) unique-pair pass, so partial correction converges over a few
    // frames rather than needing a one-shot solver - same "converges over
    // frames" reasoning §21 already established, just with the dynamic
    // part now handled by momentum instead of a bigger position snap.
    {
        float minSeparation = 1.5f * boundingSphereRadius_;
        constexpr float kPositionalCorrectionFactor = 0.1f;   // was 0.3f pre-impulse
        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            for (uint32_t j = i + 1; j < OBJECT_COUNT; j++)
            {
                glm::vec3 delta = instanceCurrentPositions_[j] - instanceCurrentPositions_[i];
                float dist = glm::length(delta);
                if (dist < minSeparation)
                {
                    glm::vec3 n = (dist > 0.001f) ? (delta / dist) : glm::vec3(0.0f, 1.0f, 0.0f);

                    // Equal-mass impulse (every grid instance shares
                    // boundingSphereRadius_, so mass is implicitly 1:1):
                    // -(1+e)*velAlongNormal/2 along the normal, only when
                    // approaching.
                    glm::vec3 relVel = instanceVelocities_[j] - instanceVelocities_[i];
                    float velAlongNormal = glm::dot(relVel, n);
                    if (velAlongNormal < 0.0f)
                    {
                        glm::vec3 impulse = -(1.0f + restitution_) * velAlongNormal * 0.5f * n;
                        instanceVelocities_[i] -= impulse;
                        instanceVelocities_[j] += impulse;
                    }

                    float overlap = minSeparation - dist;
                    instanceCurrentPositions_[i] -= n * (overlap * kPositionalCorrectionFactor);
                    instanceCurrentPositions_[j] += n * (overlap * kPositionalCorrectionFactor);
                }
            }
        }
    }

    // Re-upload the (possibly changed) positions - cheap, persistently
    // mapped memcpy (see the VulkanBuffer persistent-mapping commit).
    // A bare glm::vec4 is byte-identical to the local ComputeObjectData
    // struct above (single member, no padding), safe to upload directly.
    std::vector<glm::vec4> objects(OBJECT_COUNT);
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        objects[i] = glm::vec4(instanceCurrentPositions_[i], boundingSphereRadius_);

    objectBuffer_.upload(device_.get(), objects.data(), sizeof(glm::vec4) * OBJECT_COUNT);

    // Hierarchical culling (coarse pass): recompute each cluster's
    // enclosing bounding sphere from the live positions above, same
    // "recompute CPU-side, reupload the SSBO every frame" discipline as
    // objectBuffer_ just above - O(2*OBJECT_COUNT), negligible next to the
    // O(n^2) mutual-collision pass this function already runs. Center is
    // the member mean; radius is the max member distance-from-center plus
    // that member's own bounding radius, so the cluster sphere strictly
    // contains every member sphere (see docs/TECHNICAL_NOTES.md for why
    // that containment property is what makes the coarse gate lossless).
    {
        std::vector<glm::vec3> clusterCenterSum(CLUSTER_COUNT, glm::vec3(0.0f));
        std::vector<uint32_t>  clusterMemberCount(CLUSTER_COUNT, 0);

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            uint32_t c = clusterIndexForInstance(i);
            clusterCenterSum[c] += instanceCurrentPositions_[i];
            clusterMemberCount[c]++;
        }

        std::vector<glm::vec4> clusters(CLUSTER_COUNT, glm::vec4(0.0f));
        for (uint32_t c = 0; c < CLUSTER_COUNT; c++)
        {
            if (clusterMemberCount[c] > 0)
                clusters[c] = glm::vec4(clusterCenterSum[c] / float(clusterMemberCount[c]), 0.0f);
        }

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            uint32_t c = clusterIndexForInstance(i);
            glm::vec3 center = glm::vec3(clusters[c]);
            float reach = glm::length(instanceCurrentPositions_[i] - center) + boundingSphereRadius_;
            clusters[c].w = std::max(clusters[c].w, reach);
        }

        clusterBuffer_.upload(device_.get(), clusters.data(), sizeof(glm::vec4) * CLUSTER_COUNT);
    }
}

uint32_t VulkanContext::clusterIndexForInstance(uint32_t idx)
{
    // Must match culling.comp's GLSL recovery exactly, and
    // initSceneData()'s x/y/z generation loop order (idx = x*49+y*7+z).
    uint32_t x = idx / (GRID_SIZE * GRID_SIZE);
    uint32_t y = (idx / GRID_SIZE) % GRID_SIZE;
    uint32_t z = idx % GRID_SIZE;
    return (x / CLUSTER_DIM) * (CLUSTERS_PER_AXIS * CLUSTERS_PER_AXIS)
         + (y / CLUSTER_DIM) * CLUSTERS_PER_AXIS
         + (z / CLUSTER_DIM);
}

void VulkanContext::resetInstanceFormation()
{
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        instanceCurrentPositions_[i] = glm::vec3(cachedInstances_[i].position);
    std::fill(instanceVelocities_.begin(), instanceVelocities_.end(), glm::vec3(0.0f));
}

glm::mat4 VulkanContext::lightViewProj() const
{
    // Scene bounding radius: derived from the live instance positions
    // every frame instead of a fixed constant, so a projectile blast
    // (§20/§21 - permanent scatter, no auto-return to rest) that pushes
    // instances outward grows the light's frustum to match, rather than
    // silently clipping them out of the shadow map (and, since §28 added
    // light-frustum culling, out of the shadow pass's draw entirely).
    // kMinSceneRadius is the floor this used to be a fixed constant at:
    // the 7x7x7 grid's rest half-extent is (GRID_SIZE-1)*spacing*0.5 =
    // 9.0 per axis (spacing=3.0, see initSceneData()), a ~15.6-unit
    // half-diagonal, plus margin - so the rest-formation case (no blast
    // yet) computes the exact same 24.0 this constant used to be, making
    // this change a no-op until a blast actually happens. Recomputed
    // fresh every frame (an O(343) scan, same order as the O(n^2)
    // mutual-collision pass updateInstanceSimulation() already runs every
    // frame) rather than smoothed/quantized - accepted minor shadow-map
    // texel-density shift ("shadow swimming") in exchange for staying a
    // simple, stateless function of current positions. Grid instances
    // only; the projectile isn't included (short-lived, single instance,
    // not worth the extra coupling if its shadow gets clipped while far
    // outside the grid's footprint).
    constexpr float kMinSceneRadius = 24.0f;

    float maxDist = 0.0f;
    for (const auto& p : instanceCurrentPositions_)
        maxDist = std::max(maxDist, glm::length(p));

    float sceneRadius = std::max(kMinSceneRadius, maxDist + boundingSphereRadius_);

    glm::vec3 dir = glm::normalize(lightDirection_);
    glm::vec3 center(0.0f);
    glm::vec3 eye = center - dir * (sceneRadius * 2.0f);

    // glm::lookAt degenerates when the view direction is parallel to the
    // up vector - the default light direction points mostly straight
    // down, so fall back to a different up axis in that case.
    glm::vec3 up = (std::abs(dir.y) > 0.99f)
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    glm::mat4 view = glm::lookAt(eye, center, up);

    // orthoRH_ZO (not the plain glm::ortho()/Camera::getProjectionMatrix()
    // convention) - Vulkan needs z_ndc in [0,1], not OpenGL's [-1,1]. For a
    // *perspective* matrix that distinction only shifts the near plane by
    // a fraction of a unit (harmless, and why Camera's perspective gets
    // away with plain glm::perspective + a Y-flip) - but for an
    // *orthographic* matrix the mapping is linear, so using the [-1,1]
    // convention here would clip away the near half of the light's
    // frustum outright. This project doesn't define
    // GLM_FORCE_DEPTH_ZERO_TO_ONE globally, so the explicit *_ZO variant
    // is used instead of changing GLM's project-wide default.
    glm::mat4 proj = glm::orthoRH_ZO(
        -sceneRadius, sceneRadius,
        -sceneRadius, sceneRadius,
        0.1f, sceneRadius * 4.0f
    );
    proj[1][1] *= -1;   // Vulkan Y-flip, same as Camera::getProjectionMatrix()

    return proj * view;
}

void VulkanContext::cleanup()
{
    imguiLayer_.destroy(device_.get());
    computePipeline_.destroy(device_.get());
    computePipelineCoarse_.destroy(device_.get());
    computeDescriptor_.destroy(device_.get());
    frustumBuffer_.destroy(device_.get());
    objectBuffer_.destroy(device_.get());
    lightFrustumBuffer_.destroy(device_.get());
    shadowIndirectDrawBuffer_.destroy(device_.get());
    shadowVisibleInstanceBuffer_.destroy(device_.get());
    clusterBuffer_.destroy(device_.get());
    clusterVisibleCameraBuffer_.destroy(device_.get());
    clusterVisibleLightBuffer_.destroy(device_.get());

    skyboxPipeline_.destroy(device_.get());
    iblDescriptor_.destroy(device_.get());
    brdfLut_.destroy(device_.get());
    prefilteredCubemap_.destroy(device_.get());
    irradianceCubemap_.destroy(device_.get());
    skyboxDescriptor_.destroy(device_.get());
    environmentCubemap_.destroy(device_.get());

    for (int i = 0; i < 3; i++)
    {
        lods_[i].indirectDrawBuffer.destroy(device_.get());
        lods_[i].visibleInstanceBuffer.destroy(device_.get());
        lods_[i].indexBuffer.destroy(device_.get());
        lods_[i].vertexBuffer.destroy(device_.get());
    }

    material_.destroy(device_.get());
    projectileDescriptor_.destroy(device_.get());
    projectileUniformBuffer_.destroy(device_.get());
    projectileInstanceBuffer_.destroy(device_.get());
    sceneDataBuffer_.destroy(device_.get());
    descriptor_.destroy(device_.get());
    uniformBuffer_.destroy(device_.get());

    pipeline_.destroy(device_.get());
    sceneFramebuffer_.destroy(device_.get());
    sceneRenderPass_.destroy(device_.get());
    sceneColorDepth_.destroy(device_.get());
    sceneColorTarget_.destroy(device_.get());

    framebuffer_.destroy(device_.get());
    depthBuffer_.destroy(device_.get());
    renderPass_.destroy(device_.get());

    shadowPipeline_.destroy(device_.get());
    shadowFramebuffer_.destroy(device_.get());
    shadowRenderPass_.destroy(device_.get());
    shadowMap_.destroy(device_.get());

    swapchain_.destroy(device_.get());
    commandPool_.destroy(device_.get());

    device_.destroy();
    surface_.destroy(instance_.get());
    instance_.destroy();

    std::cout << "VulkanContext destroyed\n";
}

