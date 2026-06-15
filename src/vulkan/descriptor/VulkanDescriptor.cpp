#include "vulkan/descriptor/VulkanDescriptor.h"
#include <stdexcept>

void VulkanDescriptor::create(VkDevice device, VkBuffer uniformBuffer)
{
    createLayout(device);
    createPool(device);
    allocateAndWrite(device, uniformBuffer);
}

void VulkanDescriptor::destroy(VkDevice device)
{
    vkDestroyDescriptorPool(device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device, layout_, nullptr);
}

void VulkanDescriptor::createLayout(VkDevice device)
{
    // notify Vulkan：binding 0 is a uniform buffer use for vertex shader
    VkDescriptorSetLayoutBinding binding{};
    binding.binding            = 0;
    binding.descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount    = 1;
    binding.stageFlags         = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings    = &binding;

    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create descriptor set layout");
}

void VulkanDescriptor::createPool(VkDevice device)
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.poolSizeCount = 1;
    info.pPoolSizes    = &poolSize;
    info.maxSets       = 1;

    if (vkCreateDescriptorPool(device, &info, nullptr, &pool_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create descriptor pool");
}

void VulkanDescriptor::allocateAndWrite(VkDevice device, VkBuffer uniformBuffer)
{
    //  let pool distribute set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout_;

    if (vkAllocateDescriptorSets(device, &allocInfo, &set_) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate descriptor set");

    // write uniform buffer into set's binding 0
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = uniformBuffer;
    bufInfo.offset = 0;
    bufInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

