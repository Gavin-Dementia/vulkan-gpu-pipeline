#include "vulkan/descriptor/VulkanDescriptor.h"
#include <stdexcept>
#include <array>

void VulkanDescriptor::create(
    VkDevice device,
    VkBuffer uniformBuffer,
    VkImageView textureView,
    VkSampler textureSampler,
    VkBuffer sceneDataBuffer)
{
    createLayout(device);
    createPool(device);
    allocateAndWrite(device, uniformBuffer, textureView, textureSampler, sceneDataBuffer);
}

void VulkanDescriptor::destroy(VkDevice device)
{
    vkDestroyDescriptorPool(device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device, layout_, nullptr);
}

void VulkanDescriptor::createLayout(VkDevice device)
{
    // notify Vulkan：binding 0 is a uniform buffer use for vertex shader
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Shared per-frame scene/light data (see SceneData.h) - one binding
    // reused by every material's descriptor set, not duplicated per object.
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create descriptor set layout");
}

void VulkanDescriptor::createPool(VkDevice device)
{
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 2;   // binding 0 (MVP) + binding 2 (SceneData)
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = 1;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create descriptor pool");
}

void VulkanDescriptor::allocateAndWrite(
    VkDevice device,
    VkBuffer uniformBuffer,
    VkImageView textureView,
    VkSampler textureSampler,
    VkBuffer sceneDataBuffer
)
{
    //  let pool distribute set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout_;

    if (vkAllocateDescriptorSets(device, &allocInfo, &set_) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate descriptor set");

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = uniformBuffer;
    bufInfo.offset = 0;
    bufInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = textureView;
    imgInfo.sampler     = textureSampler;

    VkDescriptorBufferInfo sceneInfo{};
    sceneInfo.buffer = sceneDataBuffer;
    sceneInfo.offset = 0;
    sceneInfo.range  = VK_WHOLE_SIZE;

    std::array<VkWriteDescriptorSet, 3> writes{};

    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set_;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo     = &bufInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set_;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &imgInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = set_;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo     = &sceneInfo;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

