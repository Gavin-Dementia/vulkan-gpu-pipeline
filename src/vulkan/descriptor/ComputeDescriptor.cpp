#include "vulkan/descriptor/ComputeDescriptor.h"
#include <stdexcept>
#include <array>

void ComputeDescriptor::create(
    VkDevice device,
    VkBuffer objectBuffer,
    std::array<VkBuffer, 3> visibleInstanceBuffers,
    std::array<VkBuffer, 3> indirectDrawBuffers,
    VkBuffer frustumBuffer,
    VkBuffer shadowVisibleInstanceBuffer,
    VkBuffer shadowIndirectDrawBuffer,
    VkBuffer lightFrustumBuffer,
    VkBuffer clusterBuffer,
    VkBuffer clusterVisibleCameraBuffer,
    VkBuffer clusterVisibleLightBuffer,
    VkDeviceSize objectSize,
    VkDeviceSize visibleInstanceSize,  // single LOD size
    VkDeviceSize indirectDrawSize,
    VkDeviceSize frustumSize,
    VkDeviceSize clusterBufferSize,
    VkDeviceSize clusterVisibleSize
)
{
    // binding 0:  objectBuffer        (STORAGE, read)
    // binding 1:  visibleLOD0         (STORAGE, write)
    // binding 2:  visibleLOD1         (STORAGE, write)
    // binding 3:  visibleLOD2         (STORAGE, write)
    // binding 4:  indirectLOD0        (STORAGE, read/write)
    // binding 5:  indirectLOD1        (STORAGE, read/write)
    // binding 6:  indirectLOD2        (STORAGE, read/write)
    // binding 7:  frustumUBO          (UNIFORM, read)  - camera frustum
    // binding 8:  shadowVisible       (STORAGE, write) - light-frustum-culled instances
    // binding 9:  shadowIndirect      (STORAGE, read/write)
    // binding 10: lightFrustumUBO     (UNIFORM, read)  - light frustum
    // binding 11: clusterBuffer       (STORAGE, read)  - coarse pass input, 64 cluster bounding spheres
    // binding 12: clusterVisibleCamera(STORAGE, r/w)   - coarse writes, fine reads
    // binding 13: clusterVisibleLight (STORAGE, r/w)   - coarse writes, fine reads
    std::array<VkDescriptorSetLayoutBinding, 14> bindings{};
    for(int i= 0; i < 7; i++)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    bindings[7].binding         = 7;
    bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    for (int i = 8; i <= 9; i++)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    bindings[10].binding         = 10;
    bindings[10].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    for (int i = 11; i <= 13; i++)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute descriptor layout");

    // ---- Pool ----
    std::array<VkDescriptorPoolSize, 2> poolSizes{};

    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 12;

    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = 1;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute descriptor pool");

    // ---- Allocate set ----
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout_;

    if (vkAllocateDescriptorSets(device, &allocInfo, &set_) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate compute descriptor set");

    // ---- Buffer infos ----
    VkDescriptorBufferInfo objInfo{};
    objInfo.buffer = objectBuffer;
    objInfo.offset = 0;
    objInfo.range  = objectSize;

    std::array<VkDescriptorBufferInfo, 3> visInfos{};
    for (int i = 0; i < 3; i++)
    {
        visInfos[i].buffer = visibleInstanceBuffers[i];
        visInfos[i].offset = 0;
        visInfos[i].range  = visibleInstanceSize;
    }

    std::array<VkDescriptorBufferInfo, 3> indInfos{};
    for (int i = 0; i < 3; i++)
    {
        indInfos[i].buffer = indirectDrawBuffers[i];
        indInfos[i].offset = 0;
        indInfos[i].range  = indirectDrawSize;
    }

    VkDescriptorBufferInfo frustInfo{};
    frustInfo.buffer = frustumBuffer;
    frustInfo.offset = 0;
    frustInfo.range  = frustumSize;

    VkDescriptorBufferInfo shadowVisInfo{};
    shadowVisInfo.buffer = shadowVisibleInstanceBuffer;
    shadowVisInfo.offset = 0;
    shadowVisInfo.range  = visibleInstanceSize;

    VkDescriptorBufferInfo shadowIndInfo{};
    shadowIndInfo.buffer = shadowIndirectDrawBuffer;
    shadowIndInfo.offset = 0;
    shadowIndInfo.range  = indirectDrawSize;

    VkDescriptorBufferInfo lightFrustInfo{};
    lightFrustInfo.buffer = lightFrustumBuffer;
    lightFrustInfo.offset = 0;
    lightFrustInfo.range  = frustumSize;

    VkDescriptorBufferInfo clusterInfo{};
    clusterInfo.buffer = clusterBuffer;
    clusterInfo.offset = 0;
    clusterInfo.range  = clusterBufferSize;

    VkDescriptorBufferInfo clusterVisCamInfo{};
    clusterVisCamInfo.buffer = clusterVisibleCameraBuffer;
    clusterVisCamInfo.offset = 0;
    clusterVisCamInfo.range  = clusterVisibleSize;

    VkDescriptorBufferInfo clusterVisLightInfo{};
    clusterVisLightInfo.buffer = clusterVisibleLightBuffer;
    clusterVisLightInfo.offset = 0;
    clusterVisLightInfo.range  = clusterVisibleSize;

    // ---- Writes ----
    std::array<VkWriteDescriptorSet, 14> writes{};

    // binding 0: objectBuffer
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set_;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo     = &objInfo;

    // binding 1-3: visibleLOD0/1/2
    for (int i = 0; i < 3; i++)
    {
        writes[1 + i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1 + i].dstSet          = set_;
        writes[1 + i].dstBinding      = 1 + i;
        writes[1 + i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1 + i].descriptorCount = 1;
        writes[1 + i].pBufferInfo     = &visInfos[i];
    }

    // binding 4-6: indirectLOD0/1/2
    for (int i = 0; i < 3; i++)
    {
        writes[4 + i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4 + i].dstSet          = set_;
        writes[4 + i].dstBinding      = 4 + i;
        writes[4 + i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4 + i].descriptorCount = 1;
        writes[4 + i].pBufferInfo     = &indInfos[i];
    }

    // binding 7: frustumUBO
    writes[7].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet          = set_;
    writes[7].dstBinding      = 7;
    writes[7].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[7].descriptorCount = 1;
    writes[7].pBufferInfo     = &frustInfo;

    // binding 8: shadowVisible
    writes[8].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet          = set_;
    writes[8].dstBinding      = 8;
    writes[8].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].descriptorCount = 1;
    writes[8].pBufferInfo     = &shadowVisInfo;

    // binding 9: shadowIndirect
    writes[9].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet          = set_;
    writes[9].dstBinding      = 9;
    writes[9].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[9].descriptorCount = 1;
    writes[9].pBufferInfo     = &shadowIndInfo;

    // binding 10: lightFrustumUBO
    writes[10].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet          = set_;
    writes[10].dstBinding      = 10;
    writes[10].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[10].descriptorCount = 1;
    writes[10].pBufferInfo     = &lightFrustInfo;

    // binding 11: clusterBuffer
    writes[11].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[11].dstSet          = set_;
    writes[11].dstBinding      = 11;
    writes[11].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[11].descriptorCount = 1;
    writes[11].pBufferInfo     = &clusterInfo;

    // binding 12: clusterVisibleCamera
    writes[12].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet          = set_;
    writes[12].dstBinding      = 12;
    writes[12].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[12].descriptorCount = 1;
    writes[12].pBufferInfo     = &clusterVisCamInfo;

    // binding 13: clusterVisibleLight
    writes[13].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[13].dstSet          = set_;
    writes[13].dstBinding      = 13;
    writes[13].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[13].descriptorCount = 1;
    writes[13].pBufferInfo     = &clusterVisLightInfo;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void ComputeDescriptor::destroy(VkDevice device)
{
    vkDestroyDescriptorPool(device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device, layout_, nullptr);
}

