#define TINYOBJLOADER_IMPLEMENTATION
#include "vulkan/resource/ObjLoader.h"
#include "tiny_obj_loader.h"
#include <stdexcept>
#include <iostream>
#include <unordered_map>

// put Vertex into unordered_map to compare with self_define hash
struct VertexHash
{
    size_t operator()(const Vertex& v) const
    {
        size_t h1 = std::hash<float>()(v.position.x) ^ std::hash<float>()(v.position.y) ^ std::hash<float>()(v.position.z);
        size_t h2 = std::hash<float>()(v.normal.x)   ^ std::hash<float>()(v.normal.y)   ^ std::hash<float>()(v.normal.z);
        return h1 ^ (h2 << 1);
    }
};

struct VertexEqual
{
    bool operator()(const Vertex& a, const Vertex& b) const
    {
        return a.position == b.position && a.normal == b.normal;
    }
};

MeshData ObjLoader::load(const std::string& path)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str()))
        throw std::runtime_error("Failed to load OBJ: " + path + "\n" + err);

    if (!warn.empty())
        std::cout << "[ObjLoader] warn: " << warn << "\n";

    MeshData mesh;
    std::unordered_map<Vertex, uint32_t, VertexHash, VertexEqual> uniqueVertices;

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            Vertex v{};
            v.position = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            if (index.normal_index >= 0)
            {
                v.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            else
            {
                v.normal = { 0.0f, 0.0f, 1.0f };
            }

            // 如果这个顶点没出现过，加进顶点数组，记下它的index
            if (uniqueVertices.count(v) == 0)
            {
                uniqueVertices[v] = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(v);
            }

            mesh.indices.push_back(uniqueVertices[v]);
        }
    }

    std::cout << "[ObjLoader] " << mesh.vertices.size() << " unique vertices, "
               << mesh.indices.size() << " indices (from " << path << ")\n";

    return mesh;
}

