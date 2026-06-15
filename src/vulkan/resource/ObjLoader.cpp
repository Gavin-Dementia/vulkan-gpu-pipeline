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
            
            v.position = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]  
            };
            vertices.push_back(v);
        }
    }

    std::cout << "[ObjLoader] loaded " << vertices.size() << " vertices from " << path << "\n";
    
    // float minX = FLT_MAX, maxX = -FLT_MAX;
    // float minY = FLT_MAX, maxY = -FLT_MAX;
    // float minZ = FLT_MAX, maxZ = -FLT_MAX;

    // for (const auto& v : vertices) {
    //     minX = std::min(minX, v.position.x);
    //     maxX = std::max(maxX, v.position.x);
    //     minY = std::min(minY, v.position.y);
    //     maxY = std::max(maxY, v.position.y);
    //     minZ = std::min(minZ, v.position.z);
    //     maxZ = std::max(maxZ, v.position.z);
    // }    
    
    // std::cout << "[ObjLoader] X range: " << minX << " ~ " << maxX << "\n";
    // std::cout << "[ObjLoader] Y range: " << minY << " ~ " << maxY << "\n";
    // std::cout << "[ObjLoader] Z range: " << minZ << " ~ " << maxZ << "\n";
    
    return vertices;
}

