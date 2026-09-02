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
#include "vulkan/buffer/IndexBuffer.h"
#include "vulkan/instance/InstanceData.h"
#include "vulkan/buffer/IndirectDrawBuffer.h"
#include "vulkan/texture/Material.h"
#include "vulkan/lighting/SceneData.h"


class VulkanContext
{
public:
    static constexpr uint32_t GRID_SIZE = 7;
    static constexpr uint32_t OBJECT_COUNT = 
                                GRID_SIZE * GRID_SIZE * GRID_SIZE;  // 343
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
    UniformBuffer&         uniformBuffer()     { return uniformBuffer_; }
    VulkanDescriptor&      descriptor()        { return descriptor_; }
    VulkanDepthBuffer&     depthBuffer()       { return depthBuffer_; }
    VulkanComputePipeline& computePipeline()   { return computePipeline_; }
    ComputeDescriptor&     computeDescriptor() { return computeDescriptor_; }
    VulkanBuffer&          frustumBuffer()     { return frustumBuffer_; }
    VulkanBuffer&          objectBuffer()      { return objectBuffer_; }
    Material&              material()          { return material_; }

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

    // LOD distance thresholds (culling.comp's former hardcoded LOD1_DIST/
    // LOD2_DIST constants) - runtime-tunable via the "GPU Culling Stats"
    // ImGui window instead of a shader constant, same "easier to find by
    // eye than to compute, expose it" reasoning as shadowBias_ above.
    // Uploaded to the GPU every frame as part of FrustumPlanes
    // (GPUCullingPass in FrameRenderer.cpp), not a separate buffer.
    float lod1Distance() const { return lod1Distance_; }
    float lod2Distance() const { return lod2Distance_; }
    // Setters keep lod2Distance_ >= lod1Distance_ - culling.comp's
    // if/else-if chain (camDist < LOD1 -> LOD0, camDist < LOD2 -> LOD1,
    // else -> LOD2) silently misbehaves if the thresholds cross (e.g. an
    // instance between LOD2 and LOD1 would wrongly pass the first check).
    void setLod1Distance(float d)
    {
        lod1Distance_ = d;
        if (lod2Distance_ < lod1Distance_) lod2Distance_ = lod1Distance_;
    }
    void setLod2Distance(float d) { lod2Distance_ = (d < lod1Distance_) ? lod1Distance_ : d; }

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

    // Manual pause/resume for the grid's shared spin - independent of
    // the scatter system above, only affects the rotation model matrix.
    float spinAngle() const { return spinAngle_; }
    void updateSpin(float deltaTime) { if (!spinPaused_) spinAngle_ += deltaTime; }
    void toggleSpinPaused() { spinPaused_ = !spinPaused_; }

    LODMesh& lod(int level) { return lods_[level]; }


private:

    void initCore();
    void initSceneData();
    void initCullingResources();

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
    UniformBuffer         uniformBuffer_;
    VulkanDescriptor      descriptor_;
    VulkanDepthBuffer     depthBuffer_;
    VulkanBuffer          objectBuffer_;
    ComputeDescriptor     computeDescriptor_;
    VulkanComputePipeline computePipeline_;
    VulkanBuffer          frustumBuffer_;
    Material              material_;

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

    // Matches culling.comp's former hardcoded LOD1_DIST/LOD2_DIST values.
    float                 lod1Distance_   = 12.0f;
    float                 lod2Distance_   = 20.0f;
};

