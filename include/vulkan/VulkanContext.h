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
#include "vulkan/swapchain/VulkanSwapchain.h"
#include "vulkan/pipeline/VulkanPipeline.h"
#include "vulkan/pipeline/VulkanComputePipeline.h"
#include "vulkan/buffer/IndexBuffer.h"
#include "vulkan/instance/InstanceData.h"
#include "vulkan/buffer/IndirectDrawBuffer.h"
#include "vulkan/texture/VulkanTexture.h"
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
    VulkanTexture&         texture()           { return texture_; }

    VulkanBuffer&          projectileInstanceBuffer() { return projectileInstanceBuffer_; }
    UniformBuffer&         projectileUniformBuffer()  { return projectileUniformBuffer_; }
    VulkanDescriptor&      projectileDescriptor()     { return projectileDescriptor_; }

    VulkanBuffer&          sceneDataBuffer()   { return sceneDataBuffer_; }

    glm::vec3 lightDirection() const { return lightDirection_; }
    glm::vec3 lightColor()     const { return lightColor_; }
    float     lightIntensity() const { return lightIntensity_; }
    void setLightDirection(const glm::vec3& d) { lightDirection_ = d; }
    void setLightColor(const glm::vec3& c)     { lightColor_ = c; }
    void setLightIntensity(float i)            { lightIntensity_ = i; }

    // Grid collision + scatter (Phase 7 milestone 2). Re-uploads
    // objectBuffer_ every frame from CPU-simulated positions - no compute
    // shader or descriptor changes needed, since culling.comp already
    // treats whatever is in objectBuffer_ at dispatch time as ground
    // truth for both visibility and render position.
    void updateInstanceSimulation(float deltaTime);
    void resetInstanceFormation();

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
    std::vector<InstanceData> cachedInstances_;
    std::vector<glm::vec3>    instanceCurrentPositions_;  // live simulated position, size OBJECT_COUNT
    std::vector<glm::vec3>    instanceVelocities_;        // per-instance blast velocity, size OBJECT_COUNT

    std::array<LODMesh, 3> lods_;
    float boundingSphereRadius_ = 0.0f;   // computed from LOD0's mesh bounds, see initSceneData()

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
    VulkanTexture         texture_;

    VulkanBuffer          projectileInstanceBuffer_;
    UniformBuffer         projectileUniformBuffer_;
    VulkanDescriptor      projectileDescriptor_;

    VulkanBuffer          sceneDataBuffer_;
    glm::vec3             lightDirection_ = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
    glm::vec3             lightColor_     = glm::vec3(1.0f, 1.0f, 1.0f);
    float                 lightIntensity_ = 3.0f;
};

