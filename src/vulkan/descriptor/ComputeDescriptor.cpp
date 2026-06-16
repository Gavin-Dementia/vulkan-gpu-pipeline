#include "vulkan/descriptor/ComputeDescriptor.h"
#include <stdexcept>
#include <array>

void ComputeDescriptor::create(
    VkDevice device,
    VkBuffer objectBuffer,
    VkBuffer visibilityBuffer,
    VkBuffer frustumBuffer,
    VkDeviceSize objectBufferSize,
    VkDeviceSize visibilityBufferSize,
    VkDeviceSize frustumBufferSize)
{
    // ---- Layout: binding 0 = object SSBO (read), binding 1 = visibility SSBO (write) ----
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute descriptor layout");

    // ---- Pool ----
    std::array<VkDescriptorPoolSize, 2> poolSizes{};

    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 2;   // object + visibility

    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;   // frustum

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());  // 2，不是1
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

    // ---- Write both buffers into the set ----
    VkDescriptorBufferInfo objInfo{};
    objInfo.buffer = objectBuffer;
    objInfo.offset = 0;
    objInfo.range  = objectBufferSize;

    VkDescriptorBufferInfo visInfo{};
    visInfo.buffer = visibilityBuffer;
    visInfo.offset = 0;
    visInfo.range  = visibilityBufferSize;

    VkDescriptorBufferInfo frustInfo{};
    frustInfo.buffer = frustumBuffer;
    frustInfo.offset = 0;
    frustInfo.range  = frustumBufferSize;

    std::array<VkWriteDescriptorSet, 3> writes{};

    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set_;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo     = &objInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set_;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo     = &visInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = set_;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo     = &frustInfo;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void ComputeDescriptor::destroy(VkDevice device)
{
    vkDestroyDescriptorPool(device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device, layout_, nullptr);
}

