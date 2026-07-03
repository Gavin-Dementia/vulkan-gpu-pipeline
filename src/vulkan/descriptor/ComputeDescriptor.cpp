#include "vulkan/descriptor/ComputeDescriptor.h"
#include <stdexcept>
#include <array>

void ComputeDescriptor::create(
    VkDevice device,
    VkBuffer objectBuffer,
    std::array<VkBuffer, 3> visibleInstanceBuffers,
    std::array<VkBuffer, 3> indirectDrawBuffers,
    VkBuffer frustumBuffer,
    VkDeviceSize objectSize,
    VkDeviceSize visibleInstanceSize,  // single LOD size
    VkDeviceSize indirectDrawSize,
    VkDeviceSize frustumSize
)
{
    // binding 0: objectBuffer        (STORAGE, read)
    // binding 1: visibleLOD0         (STORAGE, write)
    // binding 2: visibleLOD1         (STORAGE, write)
    // binding 3: visibleLOD2         (STORAGE, write)
    // binding 4: indirectLOD0        (STORAGE, read/write)
    // binding 5: indirectLOD1        (STORAGE, read/write)
    // binding 6: indirectLOD2        (STORAGE, read/write)
    // binding 7: frustumUBO          (UNIFORM, read)
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
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

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute descriptor layout");

    // ---- Pool ----
    std::array<VkDescriptorPoolSize, 2> poolSizes{};

    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 7;

    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;   

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

    // ---- Writes ----
    std::array<VkWriteDescriptorSet, 8> writes{};

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

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void ComputeDescriptor::destroy(VkDevice device)
{
    vkDestroyDescriptorPool(device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device, layout_, nullptr);
}

