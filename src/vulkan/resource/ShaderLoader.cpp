#include "vulkan/resource/ShaderLoader.h"
#include <fstream>
#include <stdexcept>

std::vector<char> ShaderLoader::readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);

    if (!file.is_open())
        throw std::runtime_error("Failed to open shader file");

    size_t size = (size_t)file.tellg();
    std::vector<char> buffer(size);

    file.seekg(0);
    file.read(buffer.data(), size);

    file.close();
    return buffer;
}

VkShaderModule ShaderLoader::load(
    VkDevice device,
    const std::string& path)
{
    auto code = readFile(path);

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;

    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shader module");

    return module;
}

