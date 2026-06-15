#pragma once
#include <vector>
#include <string>
#include "vulkan/buffer/VertexBuffer.h"  // Vertex定义在这里

class ObjLoader
{
public:
    static std::vector<Vertex> load(const std::string& path);
};

