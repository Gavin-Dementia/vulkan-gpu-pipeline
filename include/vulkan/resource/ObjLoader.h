#pragma once
#include <vector>
#include <string>
#include "vulkan/buffer/VertexBuffer.h"  // Vertex define here

class ObjLoader
{
public:
    static std::vector<Vertex> load(const std::string& path);
};

