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

    auto vertices = ObjLoader::load("assets/suzanne.obj");
    vertexBuffer_.create(
        device_.getPhysical(),
        device_.get(),
        commandPool_.get(),
        device_.getGraphicsQueue(),
        vertices
    );

    // ---- Compute culling setup (fake data for now) ----
    struct FakeObjectData { glm::vec4 boundingSphere; };

    std::vector<FakeObjectData> fakeObjects(OBJECT_COUNT);
    for (auto& obj : fakeObjects)
        obj.boundingSphere = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // 假数据，先全填一样的

    VkDeviceSize objSize = sizeof(FakeObjectData) * OBJECT_COUNT;
    VkDeviceSize visSize = sizeof(uint32_t) * OBJECT_COUNT;

    objectBuffer_.create(
        device_.getPhysical(), device_.get(), objSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    objectBuffer_.upload(device_.get(), fakeObjects.data(), objSize);

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

    computeDescriptor_.create(
        device_.get(),
        objectBuffer_.get(),
        visibilityBuffer_.get(),
        frustumBuffer_.get(),
        objSize, visSize,
        frustumSize
    );

    computePipeline_.create(device_.get(), computeDescriptor_.layout());

    std::cout << "Vulkan Context initialized\n";
}

void VulkanContext::cleanup()
{

    computePipeline_.destroy(device_.get());
    computeDescriptor_.destroy(device_.get());
    frustumBuffer_.destroy(device_.get());
    objectBuffer_.destroy(device_.get());
    visibilityBuffer_.destroy(device_.get());

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

