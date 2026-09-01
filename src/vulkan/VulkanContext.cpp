#include "vulkan/VulkanContext.h"
#include "vulkan/resource/ObjLoader.h"
#include <glm/glm.hpp>
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

    descriptor_.create(
        device_.get(),
        uniformBuffer_.get(),
        texture_.view(),
        texture_.sampler()
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

    // No longer needed after culling resources are built
    cachedInstances_.clear();
    cachedInstances_.shrink_to_fit();
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

