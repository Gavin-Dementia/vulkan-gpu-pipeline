#include "vulkan/VulkanContext.h"
#include "vulkan/resource/ObjLoader.h"
#include <glm/glm.hpp>
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

    texture_.create(
        device_.getPhysical(), device_.get(),
        commandPool_.get(), device_.getGraphicsQueue(),
        "assets/test_texture.png"
    );

    // Shared per-frame scene/light data (SceneData.h) - one buffer bound
    // as binding 2 on every material's descriptor set, not duplicated
    // per object, since it's identical for every draw in a given frame.
    sceneDataBuffer_.create(
        device_.getPhysical(), device_.get(), sizeof(SceneData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    descriptor_.create(
        device_.get(),
        uniformBuffer_.get(),
        texture_.view(),
        texture_.sampler(),
        sceneDataBuffer_.get()
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
        texture_.view(),
        texture_.sampler(),
        sceneDataBuffer_.get()
    );

    pipeline_.create(
        device_.get(),
        swapchain_.getExtent(),
        renderPass_.get(),
        descriptor_.layout()
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
// Scene data: mesh (Vertex/Index Buffer) + instance grid
// =========================================================
void VulkanContext::initSceneData()
{
    const std::array<std::string, 3> lodPaths = {
        "assets/suzanne.obj",
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
        if (i == 0) boundingSphereRadius_ = mesh.boundingRadius;
    }

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

    objectBuffer_.create(
        device_.getPhysical(), device_.get(), objSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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

    // Frustum buffer: 6 planes + cameraPos = 7 vec4 = 112 bytes
    VkDeviceSize frustumSize = sizeof(FrustumPlanes);  
    
    frustumBuffer_.create(
        device_.getPhysical(), device_.get(), frustumSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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
        objSize, visibleInstanceSize,
        indirectDrawSize, frustumSize
    );

    computePipeline_.create(device_.get(), computeDescriptor_.layout());

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
        glm::vec3 projPos = projectile_.position();
        float hitDist = boundingSphereRadius_ + kProjectileRadius;

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            if (glm::length(instanceCurrentPositions_[i] - projPos) < hitDist)
            {
                // Blast: radial push falling off with distance from the
                // impact point, applied to every instance within range -
                // not just the one instance that was actually touched.
                for (uint32_t j = 0; j < OBJECT_COUNT; j++)
                {
                    glm::vec3 offset = instanceCurrentPositions_[j] - projPos;
                    float dist = glm::length(offset);
                    if (dist < kBlastRadius)
                    {
                        glm::vec3 dir = (dist > 0.001f) ? (offset / dist) : glm::vec3(0.0f, 1.0f, 0.0f);
                        float falloff = 1.0f - (dist / kBlastRadius);
                        instanceVelocities_[j] += dir * kImpulseStrength * falloff;
                    }
                }
                projectile_.stop();
                break;   // one explosion per flight
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
}

void VulkanContext::resetInstanceFormation()
{
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        instanceCurrentPositions_[i] = glm::vec3(cachedInstances_[i].position);
    std::fill(instanceVelocities_.begin(), instanceVelocities_.end(), glm::vec3(0.0f));
}

void VulkanContext::cleanup()
{
    imguiLayer_.destroy(device_.get());
    computePipeline_.destroy(device_.get());
    computeDescriptor_.destroy(device_.get());
    frustumBuffer_.destroy(device_.get());
    objectBuffer_.destroy(device_.get());

    for (int i = 0; i < 3; i++)
    {
        lods_[i].indirectDrawBuffer.destroy(device_.get());
        lods_[i].visibleInstanceBuffer.destroy(device_.get());
        lods_[i].indexBuffer.destroy(device_.get());
        lods_[i].vertexBuffer.destroy(device_.get());
    }

    texture_.destroy(device_.get());
    projectileDescriptor_.destroy(device_.get());
    projectileUniformBuffer_.destroy(device_.get());
    projectileInstanceBuffer_.destroy(device_.get());
    sceneDataBuffer_.destroy(device_.get());
    descriptor_.destroy(device_.get());
    uniformBuffer_.destroy(device_.get());

    pipeline_.destroy(device_.get());
    framebuffer_.destroy(device_.get());
    depthBuffer_.destroy(device_.get());
    renderPass_.destroy(device_.get());

    swapchain_.destroy(device_.get());
    commandPool_.destroy(device_.get());

    device_.destroy();
    surface_.destroy(instance_.get());
    instance_.destroy();

    std::cout << "VulkanContext destroyed\n";
}

