#pragma once
#include <vector>
#include <string>
#include "vulkan/buffer/VertexBuffer.h"

struct MeshData
{
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    // Bounding-sphere radius around the mesh's own recentered local
    // origin (see ObjLoader::load) - the tightest sphere, centered at
    // (0,0,0) in local space, that contains every vertex.
    float boundingRadius = 0.0f;
};

class ObjLoader
{
public:
    static MeshData load(const std::string& path);
};

