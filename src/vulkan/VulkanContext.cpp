#include "vulkan/VulkanContext.h"
#include "vulkan/resource/ObjLoader.h"
#include <glm/glm.hpp>
#include <iostream>
#include <vector>

void VulkanContext::init(GLFWwindow* window)
{
    window_ = window;

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

    descriptor_.create(device_.get(), uniformBuffer_.get());

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

    auto mesh = ObjLoader::load("assets/suzanne.obj");
    vertexBuffer_.create(
        device_.getPhysical(), device_.get(),
        commandPool_.get(), device_.getGraphicsQueue(),
        mesh.vertices
    );

    indexBuffer_.create(
        device_.getPhysical(), device_.get(),
        commandPool_.get(), device_.getGraphicsQueue(),
        mesh.indices
    );

    // ---- Compute culling setup ----
    // 7x7x7, blank 3.0, origin
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

    instanceBuffer_.create(
        device_.getPhysical(), device_.get(),
        commandPool_.get(), device_.getGraphicsQueue(),
        instances
    );

    struct ComputeObjectData { glm::vec4 boundingSphere; };  // xyz=center, w=radius

    std::vector<ComputeObjectData> objects(OBJECT_COUNT);
    float suzanneRadius = 1.5f;  // Suzanne大约的包围球半径

    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
    {
        objects[i].boundingSphere = glm::vec4(
            glm::vec3(instances[i].position),
            suzanneRadius
        );
    }

    VkDeviceSize objSize = sizeof(ComputeObjectData) * OBJECT_COUNT;
    VkDeviceSize visSize = sizeof(uint32_t) * OBJECT_COUNT;

    objectBuffer_.create(
        device_.getPhysical(), device_.get(), objSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    objectBuffer_.upload(device_.get(), objects.data(), objSize);

    // 输出buffer：大小跟原始instance一样大（最坏情况全部可见）
    VkDeviceSize visibleInstanceSize = sizeof(InstanceData) * OBJECT_COUNT;

    visibleInstanceBuffer_.create(
        device_.getPhysical(), device_.get(), visibleInstanceSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,  // 双重用途
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    indirectDrawBuffer_.create(
        device_.getPhysical(), device_.get(),
        static_cast<uint32_t>(mesh.indices.size())  // 用Suzanne的真实index count
    );

    visibilityBuffer_.create(
        device_.getPhysical(), device_.get(), visSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Frustum buffer: vec4平面*6个 = 96 bytes
    VkDeviceSize frustumSize = sizeof(glm::vec4) * 6;

    frustumBuffer_.create(
        device_.getPhysical(), device_.get(), frustumSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    VkDeviceSize indirectDrawSize= sizeof(VkDrawIndexedIndirectCommand);

    computeDescriptor_.create(
        device_.get(),
        objectBuffer_.get(),
        visibleInstanceBuffer_.get(),
        frustumBuffer_.get(),
        indirectDrawBuffer_.get(),
        objSize, visibleInstanceSize,
        frustumSize, indirectDrawSize
    );

    computePipeline_.create(device_.get(), computeDescriptor_.layout());

    std::cout << "Vulkan Context initialized\n";
}

void VulkanContext::cleanup()
{

    computePipeline_.destroy(device_.get());
    computeDescriptor_.destroy(device_.get());
    frustumBuffer_.destroy(device_.get());
    visibilityBuffer_.destroy(device_.get());
    visibleInstanceBuffer_.destroy(device_.get());
    indirectDrawBuffer_.destroy(device_.get());
    objectBuffer_.destroy(device_.get());
    instanceBuffer_.destroy(device_.get());
    indexBuffer_.destroy(device_.get());
    vertexBuffer_.destroy(device_.get());
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

