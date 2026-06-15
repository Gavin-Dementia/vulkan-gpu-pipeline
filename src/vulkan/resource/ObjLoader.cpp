#define TINYOBJLOADER_IMPLEMENTATION
#include "vulkan/resource/ObjLoader.h"
#include "tiny_obj_loader.h"
#include <cfloat>
#include <stdexcept>
#include <filesystem>
#include <iostream>

std::vector<Vertex> ObjLoader::load(const std::string& path)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    std::cout << "[ObjLoader] looking for: " << path << "\n";
    std::cout << "[ObjLoader] cwd: " << std::filesystem::current_path() << "\n";

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str()))
        throw std::runtime_error("Failed to load OBJ: " + path + "\n" + err);

    if (!warn.empty())
        std::cout << "[ObjLoader] warn: " << warn << "\n";

    std::vector<Vertex> vertices;

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            Vertex v{};
            // OBJ的顶点坐标是3D的，我们先只取XY（等下一步加了Z再改）
            v.position = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1]
            };
            vertices.push_back(v);
        }
    }

    std::cout << "[ObjLoader] loaded " << vertices.size() << " vertices from " << path << "\n";
    
    //歸一化
    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;

    for (const auto& v : vertices) {
        minX = std::min(minX, v.position.x);
        maxX = std::max(maxX, v.position.x);
        minY = std::min(minY, v.position.y);
        maxY = std::max(maxY, v.position.y);
    }    
    float cx = (minX + maxX) * 0.5f;
    float cy = (minY + maxY) * 0.5f;
    float scale = 0.8f / std::max((maxX - minX), (maxY - minY)) * 2.0f;

    for (auto& v : vertices) {
        v.position.x = (v.position.x - cx) * scale;
        v.position.y = (v.position.y - cy) * scale;
    }

    std::cout << "[ObjLoader] X range: " << minX << " ~ " << maxX << "\n";
    std::cout << "[ObjLoader] Y range: " << minY << " ~ " << maxY << "\n";
    
    return vertices;
}

