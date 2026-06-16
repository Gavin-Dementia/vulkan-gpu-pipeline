#pragma once
#include <vector>
#include <string>
#include "vulkan/buffer/VertexBuffer.h"

struct MeshData
{
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

class ObjLoader
{
public:
    static MeshData load(const std::string& path);
};

