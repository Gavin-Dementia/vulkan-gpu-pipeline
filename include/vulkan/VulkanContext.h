#pragma once

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include "core/Camera.h"
#include "core/Projectile.h"
#include "ui/ImGuiLayer.h"

#include "vulkan/buffer/VertexBuffer.h"
#include "vulkan/buffer/UniformBuffer.h"
#include "vulkan/command/VulkanCommandPool.h"
#include "vulkan/core/VulkanInstance.h"
#include "vulkan/culling/Frustum.h"
#include "vulkan/device/VulkanDevice.h"
#include "vulkan/descriptor/ComputeDescriptor.h"
#include "vulkan/descriptor/VulkanDescriptor.h"
#include "vulkan/platform/VulkanSurface.h"
#include "vulkan/renderpass/VulkanDepthBuffer.h"
#include "vulkan/renderpass/VulkanFramebuffer.h"
#include "vulkan/renderpass/VulkanRenderPass.h"
#include "vulkan/renderpass/VulkanShadowMap.h"
#include "vulkan/renderpass/VulkanSceneColorTarget.h"
#include "vulkan/swapchain/VulkanSwapchain.h"
#include "vulkan/pipeline/VulkanPipeline.h"
#include "vulkan/pipeline/VulkanComputePipeline.h"
#include "vulkan/pipeline/VulkanShadowPipeline.h"
#include "vulkan/pipeline/VulkanSkyboxPipeline.h"
#include "vulkan/buffer/IndexBuffer.h"
#include "vulkan/instance/InstanceData.h"
#include "vulkan/buffer/IndirectDrawBuffer.h"
#include "vulkan/texture/Material.h"
#include "vulkan/texture/VulkanCubemap.h"
#include "vulkan/texture/VulkanBRDFLut.h"
#include "vulkan/descriptor/CubeSamplerDescriptor.h"
#include "vulkan/descriptor/IBLDescriptor.h"
#include "vulkan/lighting/SceneData.h"


class VulkanContext
{
public:
    static constexpr uint32_t GRID_SIZE = 7;
    static constexpr uint32_t OBJECT_COUNT =
                                GRID_SIZE * GRID_SIZE * GRID_SIZE;  // 343

    // Hierarchical / two-stage GPU culling (coarse cluster pass + the
    // existing fine per-object pass) - see architecture.md. Groups the
    // grid into CLUSTER_DIM^3-cell clusters; CLUSTER_DIM=2 on a 7-wide
    // grid gives 4 clusters/axis (uneven membership at the edges, fine -
    // the coarse pass never needs uniform cluster size).
    static constexpr uint32_t CLUSTER_DIM = 2;
    static constexpr uint32_t CLUSTERS_PER_AXIS = (GRID_SIZE + CLUSTER_DIM - 1) / CLUSTER_DIM;
    static constexpr uint32_t CLUSTER_COUNT =
                                CLUSTERS_PER_AXIS * CLUSTERS_PER_AXIS * CLUSTERS_PER_AXIS;  // 64
    struct LODMesh
    {
        VertexBuffer   vertexBuffer;
        IndexBuffer    indexBuffer;
        VulkanBuffer   visibleInstanceBuffer;
        IndirectDrawBuffer indirectDrawBuffer;
    };

    std::array<uint32_t, 3> lastVisibleCount_ = {0, 0, 0};
    void setLastVisibleCount(int lod, uint32_t c)   { lastVisibleCount_[lod] = c; }
    uint32_t getLastVisibleCount(int lod) const     { return lastVisibleCount_[lod]; }

    // Coarse-pass cluster counts (out of CLUSTER_COUNT) - read back the
    // same safe post-fence-wait way as lastVisibleCount_ above, for the
    // "GPU Culling Stats" ImGui window.
    void setLastClusterVisibleCounts(uint32_t camera, uint32_t light)
    {
        lastClusterVisibleCamera_ = camera;
        lastClusterVisibleLight_  = light;
    }
    uint32_t getLastClusterVisibleCamera() const { return lastClusterVisibleCamera_; }
    uint32_t getLastClusterVisibleLight()  const { return lastClusterVisibleLight_; }

    // GPU timing (ms), read back from the previous use of the current
    // frame slot's timestamp query pool - see FrameRenderer::drawFrame().
    // graphicsMs covers GeometryPass + ImGuiPass combined (not split
    // further - ImGui's own cost isn't worth a 5th timestamp marker).
    struct GpuTiming
    {
        float cullingMs  = 0.0f;
        float shadowMs   = 0.0f;
        float graphicsMs = 0.0f;
        float totalMs    = 0.0f;
    };
    void setGpuTiming(const GpuTiming& t) { gpuTiming_ = t; }
    const GpuTiming& gpuTiming() const    { return gpuTiming_; }

    void init(GLFWwindow* window);
    void cleanup();

    GLFWwindow* window()     { return window_; }
    Camera&     camera()     { return camera_; }
    Projectile& projectile() { return projectile_; }
    ImGuiLayer& imguiLayer() { return imguiLayer_; }

    VulkanDevice&          device()            { return device_; }
    VulkanSwapchain&       swapchain()         { return swapchain_; }
    VulkanInstance&        instance()          { return instance_; }
    VulkanSurface&         surface()           { return surface_; }
    VulkanCommandPool&     commandPool()       { return commandPool_; }
    VulkanRenderPass&      renderPass()        { return renderPass_; }
    VulkanFramebuffer&     framebuffer()       { return framebuffer_; }
    VulkanPipeline&        pipeline()          { return pipeline_; }
    // Alpha-blended sibling of pipeline_ - same shaders/descriptor
    // layout, blendEnable=true and depthWriteEnable=false instead (see
    // VulkanPipeline::create()'s transparent parameter and
    // docs/TECHNICAL_NOTES.md §43). Bound by GeometryPass instead of
    // pipeline_ whenever gridAlpha() < 1.0. Targets sceneRenderPass_/
    // sceneColorTarget_ just like pipeline_, so resizeSceneTarget() must
    // recreate this too.
    VulkanPipeline&        transparentPipeline() { return transparentPipeline_; }
    UniformBuffer&         uniformBuffer()     { return uniformBuffer_; }
    VulkanDescriptor&      descriptor()        { return descriptor_; }
    VulkanDepthBuffer&     depthBuffer()       { return depthBuffer_; }
    VulkanComputePipeline& computePipeline()   { return computePipeline_; }
    // Coarse cluster-culling pass - see "Hierarchical / multi-pass GPU
    // culling" in architecture.md. Shares computeDescriptor_'s set/layout
    // with computePipeline_ (the fine pass) - both dispatches happen back
    // to back inside GPUCullingPass in FrameRenderer.cpp.
    VulkanComputePipeline& computePipelineCoarse() { return computePipelineCoarse_; }
    // Back-to-front sort of each LOD bucket's compacted visible-instance
    // list, keyed on the camera distance culling.comp writes into
    // InstanceData.position.w - see sortInstances.comp and
    // docs/TECHNICAL_NOTES.md §43. Shares computeDescriptor_'s set/layout
    // like computePipelineCoarse_ does; dispatched by GPUCullingPass
    // right after the fine pass, only when transparencySortEnabled() and
    // gridAlpha() < 1.0 (a no-op, unreached dispatch otherwise).
    VulkanComputePipeline& sortPipeline()       { return sortPipeline_; }
    ComputeDescriptor&     computeDescriptor() { return computeDescriptor_; }
    VulkanBuffer&          frustumBuffer()     { return frustumBuffer_; }
    VulkanBuffer&          objectBuffer()      { return objectBuffer_; }
    Material&              material()          { return material_; }

    // IBL Milestone 1 (see docs/TECHNICAL_NOTES.md §33) - a procedurally
    // baked environment cubemap, sampled by the live skybox draw
    // (GeometryPass in FrameRenderer.cpp), the irradiance convolution
    // (M2), and the specular prefilter (M3).
    VulkanCubemap&           environmentCubemap() { return environmentCubemap_; }
    VulkanSkyboxPipeline&    skyboxPipeline()     { return skyboxPipeline_; }
    CubeSamplerDescriptor&   skyboxDescriptor()   { return skyboxDescriptor_; }

    // IBL Milestone 2 (see docs/TECHNICAL_NOTES.md §34) - cosine-weighted
    // diffuse irradiance, convolved from environmentCubemap_ once at
    // startup.
    VulkanCubemap&           irradianceCubemap()   { return irradianceCubemap_; }

    // IBL Milestone 3 (see docs/TECHNICAL_NOTES.md §35) - GGX-importance-
    // sampled prefiltered specular radiance (5 mips, one per roughness
    // band) + the BRDF integration LUT, Karis's split-sum specular IBL
    // approximation. iblDescriptor_ bundles irradianceCubemap_ (M2) +
    // prefilteredCubemap_ + brdfLut_ (M3) into one 3-binding set, bound
    // as descriptor set 1 on the main graphics pipeline (pipeline_) -
    // shared by both the grid and the projectile, since ambient lighting
    // is scene-wide, not per-material. triangle.frag samples all three
    // to compute both halves of the ambient term - specular IBL is now
    // complete, closing the roadmap's IBL gap.
    VulkanCubemap&           prefilteredCubemap()  { return prefilteredCubemap_; }
    VulkanBRDFLut&           brdfLut()             { return brdfLut_; }
    IBLDescriptor&           iblDescriptor()       { return iblDescriptor_; }

    // Hierarchical culling's per-frame cluster bounding spheres (CPU-
    // aggregated from instanceCurrentPositions_ in updateInstanceSimulation(),
    // same "recompute + reupload every frame" discipline objectBuffer_
    // already uses) and the coarse pass's two output flag buffers.
    VulkanBuffer&          clusterBuffer()             { return clusterBuffer_; }
    VulkanBuffer&          clusterVisibleCameraBuffer() { return clusterVisibleCameraBuffer_; }
    VulkanBuffer&          clusterVisibleLightBuffer()  { return clusterVisibleLightBuffer_; }

    // Light-frustum culling for the shadow pass - same shared
    // objectBuffer_/culling.comp dispatch as the camera path, just a
    // second independent 6-plane test (light frustum, not camera frustum)
    // whose survivors get compacted into their own buffer/indirect-draw
    // pair, mirroring the {visibleInstanceBuffer, indirectDrawBuffer}
    // shape lods_[] already uses. See "Shadow mapping" in
    // architecture.md.
    VulkanBuffer&          shadowVisibleInstanceBuffer() { return shadowVisibleInstanceBuffer_; }
    IndirectDrawBuffer&    shadowIndirectDrawBuffer()    { return shadowIndirectDrawBuffer_; }
    VulkanBuffer&          lightFrustumBuffer()          { return lightFrustumBuffer_; }

    VulkanBuffer&          projectileInstanceBuffer() { return projectileInstanceBuffer_; }
    UniformBuffer&         projectileUniformBuffer()  { return projectileUniformBuffer_; }
    VulkanDescriptor&      projectileDescriptor()     { return projectileDescriptor_; }

    VulkanBuffer&          sceneDataBuffer()   { return sceneDataBuffer_; }

    VulkanShadowMap&        shadowMap()          { return shadowMap_; }
    VulkanRenderPass&       shadowRenderPass()   { return shadowRenderPass_; }
    VulkanFramebuffer&      shadowFramebuffer()  { return shadowFramebuffer_; }
    VulkanShadowPipeline&   shadowPipeline()     { return shadowPipeline_; }

    // Offscreen scene render target for the dockable ImGui "Viewport"
    // panel - GeometryPass/LightingPass/PostProcess (PassStage::Graphics)
    // now render here instead of directly to the swapchain; the swapchain's
    // own renderPass_/framebuffer_ host only the UI-stage pass. See
    // architecture.md's "Dockable viewport" module notes.
    VulkanSceneColorTarget& sceneColorTarget()   { return sceneColorTarget_; }
    VulkanRenderPass&       sceneRenderPass()    { return sceneRenderPass_; }
    VulkanFramebuffer&      sceneFramebuffer()   { return sceneFramebuffer_; }

    // Recreates sceneColorTarget_/sceneColorDepth_/sceneFramebuffer_ and
    // the two pipelines whose VkViewport/scissor is baked in at creation
    // time (pipeline_, skyboxPipeline_) at a new size - called by
    // FrameRenderer when the docked "Viewport" panel is resized, see
    // docs/TECHNICAL_NOTES.md §36. sceneRenderPass_ is untouched (a
    // VkRenderPass doesn't encode extent). Blocks on vkDeviceWaitIdle -
    // these resources are shared across both frames-in-flight, not
    // per-slot, so nothing lighter-weight is provably sufficient. Clamps
    // width/height to a 64px floor itself, the single authoritative
    // enforcement point (same "a public setter clamps its own invariant"
    // convention as setLod1ScreenSize()/setRestitution() below) - callers
    // don't need to pre-clamp. No-ops if width/height already match the
    // current size.
    void resizeSceneTarget(uint32_t width, uint32_t height);

    // Live-resized window/swapchain (see docs/TECHNICAL_NOTES.md §39).
    // Recreates framebuffer_/depthBuffer_/swapchain_ at whatever size
    // glfwGetFramebufferSize(window_) currently reports - unlike
    // resizeSceneTarget() above, no width/height parameters: the
    // swapchain's extent always tracks the actual window, there's no
    // independent "requested size" to pass in. renderPass_ is untouched
    // (format doesn't change on a plain resize, and a VkRenderPass
    // doesn't encode extent - same fact resizeSceneTarget() relies on for
    // sceneRenderPass_); pipeline_/skyboxPipeline_ need no changes either,
    // since both target the offscreen sceneRenderPass_/sceneColorTarget_,
    // not the swapchain, and never bake the swapchain's extent into a
    // VkViewport. Blocks on vkDeviceWaitIdle, same "simplest correct
    // implementation, no debounce" tradeoff as resizeSceneTarget(). Called
    // by FrameRenderer at the top of drawFrame(), never mid-frame.
    void resizeSwapchain();

    // Directional light's orthographic view-projection matrix, framed
    // around a fixed scene-radius constant covering the 7x7x7 grid's
    // footprint plus scatter margin - single source of truth, same
    // discipline as Camera::getProjectionMatrix() (see architecture.md).
    glm::mat4 lightViewProj() const;

    glm::vec3 lightDirection() const { return lightDirection_; }
    glm::vec3 lightColor()     const { return lightColor_; }
    float     lightIntensity() const { return lightIntensity_; }
    void setLightDirection(const glm::vec3& d) { lightDirection_ = d; }
    void setLightColor(const glm::vec3& c)     { lightColor_ = c; }
    void setLightIntensity(float i)            { lightIntensity_ = i; }

    // Base shadow bias (triangle.frag scales it by 1-dot(N,L), see
    // calcShadow()) - runtime-tunable via the "Lighting" ImGui window
    // instead of a shader constant, since the right value depends on
    // shadow map resolution/scene scale and is easiest to find by eye.
    float shadowBias() const          { return shadowBias_; }
    void  setShadowBias(float b)      { shadowBias_ = b; }

    // LOD selection thresholds, in screen-space projected pixels rather
    // than world-space distance (see docs/TECHNICAL_NOTES.md for the
    // transition) - runtime-tunable via the "GPU Culling Stats" ImGui
    // window, same "easier to find by eye than to compute, expose it"
    // reasoning as shadowBias_ above. Uploaded to the GPU every frame as
    // part of FrustumPlanes::lodParams (GPUCullingPass in
    // FrameRenderer.cpp), not a separate buffer; culling.comp derives an
    // object's approximate on-screen size from its bounding sphere,
    // camera distance, and the frame's screen-projection scale, then
    // compares against these thresholds.
    float lod1ScreenSize() const { return lod1ScreenSize_; }
    float lod2ScreenSize() const { return lod2ScreenSize_; }
    // Setters keep lod1ScreenSize_ >= lod2ScreenSize_ - the inverse of the
    // old world-distance invariant, since screen size shrinks as distance
    // grows: LOD1's threshold (the LOD0/LOD1 boundary, closer/bigger) must
    // stay the larger pixel value, LOD2's (the LOD1/LOD2 boundary,
    // farther/smaller) the smaller one. culling.comp's if/else-if chain
    // (screenSize > LOD1 -> LOD0, screenSize > LOD2 -> LOD1, else -> LOD2)
    // silently misbehaves if the thresholds cross.
    void setLod1ScreenSize(float s)
    {
        lod1ScreenSize_ = s;
        if (lod2ScreenSize_ > lod1ScreenSize_) lod2ScreenSize_ = lod1ScreenSize_;
    }
    void setLod2ScreenSize(float s) { lod2ScreenSize_ = (s > lod1ScreenSize_) ? lod1ScreenSize_ : s; }

    // Mesh-detail-derived LOD2 threshold default (see docs/TECHNICAL_NOTES.md
    // §40). Raw triangle counts, captured once in initSceneData() right
    // after each LOD's ObjLoader::load() - exposed for the "GPU Culling
    // Stats" ImGui window's transparency display, not used anywhere else.
    uint32_t lod0TriangleCount() const { return lod0TriangleCount_; }
    uint32_t lod1TriangleCount() const { return lod1TriangleCount_; }
    uint32_t lod2TriangleCount() const { return lod2TriangleCount_; }

    // How much simpler LOD2 is than LOD1, by triangle count - the signal
    // initSceneData() uses to derive lod2ScreenSize_'s startup default
    // (lod1ScreenSize_ * this ratio) instead of an independent hand-picked
    // constant. lod1ScreenSize_ has no equivalent derivation - there's no
    // "LOD -1" mesh to compare LOD0 against, so it stays the one manually-
    // anchored top-level threshold.
    float lod2DetailRatio() const
    {
        return lod1TriangleCount_ > 0
            ? static_cast<float>(lod2TriangleCount_) / static_cast<float>(lod1TriangleCount_)
            : 1.0f;
    }

    // Re-derives lod2ScreenSize_ from lod1ScreenSize_ * lod2DetailRatio() -
    // the same formula initSceneData() uses for the startup default, wired
    // to a "Reset to mesh-derived default" button in the "GPU Culling
    // Stats" ImGui window so a manually-dragged slider value can always be
    // recovered. Routes through setLod2ScreenSize() so the lod1 >= lod2
    // invariant above is still enforced.
    void resetLod2ScreenSizeToMeshDefault() { setLod2ScreenSize(lod1ScreenSize_ * lod2DetailRatio()); }

    // Grid/projectile opacity (see docs/TECHNICAL_NOTES.md §43) - drives
    // both the material push constant's alpha (triangle.frag) and which
    // pipeline GeometryPass binds: pipeline_ (opaque) at 1.0, or
    // transparentPipeline_ (alpha-blended) for anything less. Default 1.0
    // keeps existing behavior byte-for-byte unchanged - the transparent
    // path is entirely opt-in.
    float gridAlpha() const { return gridAlpha_; }
    void  setGridAlpha(float a) { gridAlpha_ = glm::clamp(a, 0.0f, 1.0f); }
    bool  isTransparent() const { return gridAlpha_ < 1.0f; }

    // Gates sortInstances.comp's dispatch (see sortPipeline() above) -
    // default on, so transparency looks correct out of the box; exposed
    // as a toggle purely to demonstrate the blending-order bug it fixes,
    // same "prove the mechanism, verify by eye" pattern as
    // clampDeltaTimeEnabled_.
    bool  transparencySortEnabled() const { return transparencySortEnabled_; }
    void  setTransparencySortEnabled(bool e) { transparencySortEnabled_ = e; }

    // Master texture toggle (§44) - default off, so the grid/projectile
    // render with Phase 8 milestone 1's flat, push-constant-only PBR
    // shading (real lighting/shadows/IBL, no material texture detail)
    // until explicitly enabled. Drives MaterialPushConstants::
    // metallicRoughness.z, which triangle.frag branches on.
    bool  texturesEnabled() const { return texturesEnabled_; }
    void  setTexturesEnabled(bool e) { texturesEnabled_ = e; }

    // Grid collision + scatter (Phase 7 milestone 2). Re-uploads
    // objectBuffer_ every frame from CPU-simulated positions - no compute
    // shader or descriptor changes needed, since culling.comp already
    // treats whatever is in objectBuffer_ at dispatch time as ground
    // truth for both visibility and render position.
    void updateInstanceSimulation(float deltaTime);
    void resetInstanceFormation();

    // Collision volume, independent of the render/culling bounding
    // sphere (boundingSphereRadius_) - starts equal to it (a sensible,
    // mesh-derived default) but can diverge, e.g. a more forgiving hit
    // radius than the visual mesh. culling.comp is unaffected; only
    // updateInstanceSimulation()'s hit test reads this.
    float collisionRadius() const { return collisionRadius_; }
    void  setCollisionRadius(float r) { collisionRadius_ = r; }

    // Restitution (bounciness) for mutual instance-vs-instance collision
    // response - 0 = fully inelastic (no bounce), 1 = fully elastic.
    // Runtime-tunable via the "GPU Culling Stats" ImGui window, same
    // "easier to find the right feel by eye than to compute" reasoning as
    // shadowBias_/lod1ScreenSize_/lod2ScreenSize_. Only used by
    // updateInstanceSimulation()'s mutual-collision impulse (see
    // TECHNICAL_NOTES.md §30); clamped since values outside [0,1] are
    // physically meaningless.
    float restitution() const { return restitution_; }
    void  setRestitution(float e) { restitution_ = glm::clamp(e, 0.0f, 1.0f); }

    // Interactive deltaTime clamp (see docs/TECHNICAL_NOTES.md §37) -
    // off by default, preserving this project's original uncapped-
    // deltaTime behavior. When enabled, Application::mainLoop() caps
    // deltaTime at maxDeltaTime_ before any consumer (Camera::processInput,
    // Projectile::update, updateInstanceSimulation, updateSpin) ever sees
    // it - the single authoritative "prevent spiral of death" clamp
    // point, same "a public setter clamps its own invariant" convention
    // as setRestitution()/setLod1ScreenSize() above. Runtime-tunable via
    // the "GPU Culling Stats" ImGui window, doubling as an interactive
    // demonstration of §37's diagnosis - toggle it live at high
    // restitution to see the difference a resize-triggered deltaTime
    // spike makes with and without the clamp.
    bool  clampDeltaTimeEnabled() const { return clampDeltaTimeEnabled_; }
    void  setClampDeltaTimeEnabled(bool enabled) { clampDeltaTimeEnabled_ = enabled; }
    float maxDeltaTime() const { return maxDeltaTime_; }
    void  setMaxDeltaTime(float d) { maxDeltaTime_ = glm::clamp(d, 1.0f / 240.0f, 1.0f); }

    // Manual pause/resume for the grid's shared spin - independent of
    // the scatter system above, only affects the rotation model matrix.
    float spinAngle() const { return spinAngle_; }
    void updateSpin(float deltaTime) { if (!spinPaused_) spinAngle_ += deltaTime; }
    void toggleSpinPaused() { spinPaused_ = !spinPaused_; }

    LODMesh& lod(int level) { return lods_[level]; }


private:

    void initCore();
    // IBL Milestones 1-3 (see docs/TECHNICAL_NOTES.md §33/§34/§35) -
    // creates environmentCubemap_/irradianceCubemap_/prefilteredCubemap_/
    // brdfLut_ and bakes all four (one one-shot command buffer: 6
    // procedural-sky draws, a barrier, 6 irradiance-convolution draws, a
    // barrier, 30 specular-prefilter draws (5 mips x 6 faces), a barrier,
    // 1 BRDF LUT draw, a barrier), using locally-scoped render pass(es)/
    // framebuffers/pipelines destroyed before this function returns.
    // Also creates the persistent skyboxDescriptor_/iblDescriptor_/
    // skyboxPipeline_. Called from inside initCore() itself (not from
    // init()) - right after sceneFramebuffer_.create() (needs
    // sceneRenderPass_/sceneColorTarget_) and right before
    // pipeline_.create() (needs iblDescriptor_.layout() as its second
    // descriptor set).
    void initEnvironment();
    void initSceneData();
    void initCullingResources();

    // Cluster index for a linear grid instance index - must match
    // culling.comp's GLSL recovery exactly (see that shader's comment)
    // and initSceneData()'s x/y/z generation loop order.
    static uint32_t clusterIndexForInstance(uint32_t idx);

    // Grid's rest formation (7x7x7 grid positions computed in
    // initSceneData()) - kept alive for the app's lifetime (not cleared
    // after init) as the reference resetInstanceFormation() restores.
    GpuTiming gpuTiming_;

    std::vector<InstanceData> cachedInstances_;
    std::vector<glm::vec3>    instanceCurrentPositions_;  // live simulated position, size OBJECT_COUNT
    std::vector<glm::vec3>    instanceVelocities_;        // per-instance blast velocity, size OBJECT_COUNT

    std::array<LODMesh, 3> lods_;
    float boundingSphereRadius_ = 0.0f;   // computed from LOD0's mesh bounds, see initSceneData()
    float collisionRadius_      = 0.0f;   // independent of boundingSphereRadius_, see accessor comment above
    float restitution_          = 0.3f;   // mutual-collision bounciness, see accessor comment above

    bool  clampDeltaTimeEnabled_ = false;      // off by default - see accessor comment above
    float maxDeltaTime_          = 1.0f / 15.0f;   // standard real-time-loop "spiral of death" ceiling

    float spinAngle_    = 0.0f;   // accumulated grid rotation angle, replaces raw glfwGetTime()
    bool  spinPaused_   = false;

    GLFWwindow* window_ = nullptr;
    Camera camera_;
    Projectile projectile_;
    ImGuiLayer imguiLayer_;

    VulkanInstance        instance_;
    VulkanSurface         surface_;
    VulkanDevice          device_;
    VulkanSwapchain       swapchain_;
    VulkanCommandPool     commandPool_;
    VulkanRenderPass      renderPass_;
    VulkanFramebuffer     framebuffer_;
    VulkanPipeline        pipeline_;
    VulkanPipeline        transparentPipeline_;   // see accessor comment above (§43)
    UniformBuffer         uniformBuffer_;
    VulkanDescriptor      descriptor_;
    VulkanDepthBuffer     depthBuffer_;
    VulkanBuffer          objectBuffer_;
    ComputeDescriptor     computeDescriptor_;
    VulkanComputePipeline computePipeline_;
    VulkanComputePipeline computePipelineCoarse_;
    VulkanComputePipeline sortPipeline_;          // see accessor comment above (§43)
    VulkanBuffer          frustumBuffer_;
    Material              material_;

    // IBL Milestones 1-3 - see accessor comments above.
    VulkanCubemap          environmentCubemap_;
    VulkanSkyboxPipeline   skyboxPipeline_;
    CubeSamplerDescriptor  skyboxDescriptor_;
    VulkanCubemap          irradianceCubemap_;
    VulkanCubemap          prefilteredCubemap_;
    VulkanBRDFLut          brdfLut_;
    IBLDescriptor          iblDescriptor_;

    // Hierarchical culling (coarse pass) - see accessor comments above.
    VulkanBuffer          clusterBuffer_;
    VulkanBuffer          clusterVisibleCameraBuffer_;
    VulkanBuffer          clusterVisibleLightBuffer_;
    uint32_t              lastClusterVisibleCamera_ = 0;
    uint32_t              lastClusterVisibleLight_  = 0;

    // Shadow pass's light-frustum-culled instance set - see accessor
    // comments above.
    VulkanBuffer          shadowVisibleInstanceBuffer_;
    IndirectDrawBuffer    shadowIndirectDrawBuffer_;
    VulkanBuffer          lightFrustumBuffer_;

    VulkanBuffer          projectileInstanceBuffer_;
    UniformBuffer         projectileUniformBuffer_;
    VulkanDescriptor      projectileDescriptor_;

    VulkanBuffer          sceneDataBuffer_;

    VulkanShadowMap       shadowMap_;
    VulkanRenderPass      shadowRenderPass_;
    VulkanFramebuffer     shadowFramebuffer_;
    VulkanShadowPipeline  shadowPipeline_;

    VulkanSceneColorTarget sceneColorTarget_;
    VulkanDepthBuffer      sceneColorDepth_;   // independent of the swapchain's depthBuffer_
    VulkanRenderPass       sceneRenderPass_;
    VulkanFramebuffer      sceneFramebuffer_;

    glm::vec3             lightDirection_ = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
    glm::vec3             lightColor_     = glm::vec3(1.0f, 1.0f, 1.0f);
    float                 lightIntensity_ = 3.0f;
    float                 shadowBias_     = 0.002f;

    // Screen-space LOD thresholds, in projected pixels (see accessor
    // comments above). lod1ScreenSize_'s default was chosen to roughly
    // match the old flat 12.0 world-space distance constant at typical
    // grid-viewing ranges; lod2ScreenSize_'s literal here is just a
    // placeholder - initSceneData() overwrites it with a mesh-detail-
    // derived value (lod1ScreenSize_ * lod2DetailRatio()) once the LOD
    // meshes' triangle counts are known (see docs/TECHNICAL_NOTES.md §40).
    float                 lod1ScreenSize_ = 120.0f;
    float                 lod2ScreenSize_ = 60.0f;

    // Mesh-detail-derived LOD2 threshold (§40) - raw triangle counts per
    // LOD, captured once in initSceneData(). See lod2DetailRatio() above.
    uint32_t              lod0TriangleCount_ = 0;
    uint32_t              lod1TriangleCount_ = 0;
    uint32_t              lod2TriangleCount_ = 0;

    // Transparency (§43) - see accessor comments above.
    float                 gridAlpha_ = 1.0f;
    bool                  transparencySortEnabled_ = true;

    // Master texture toggle (§44) - see accessor comment above. Default
    // off - existing behavior since Phase 8 milestone 2 was always-on,
    // so this is a deliberate, visible default change, not a no-op one.
    bool                  texturesEnabled_ = false;
};

