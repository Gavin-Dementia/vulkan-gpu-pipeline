#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

class ShaderLoader
{
public:
    static VkShaderModule load(
        VkDevice device,
        const std::string& path);

private:
    static std::vector<char> readFile(const std::string& path);
};

