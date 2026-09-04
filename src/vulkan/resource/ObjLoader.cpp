#define TINYOBJLOADER_IMPLEMENTATION
#include "vulkan/resource/ObjLoader.h"
#include "tiny_obj_loader.h"
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <limits>
#include <algorithm>

// boost::hash_combine-style mix: plain XOR is commutative, so e.g.
// position (1,2,3) and (2,1,3) would hash identically under
// h(x)^h(y)^h(z). The magic constant (fractional part of the golden
// ratio, in Q32 fixed point) plus the shifts make the combine
// order-dependent and spread bits more evenly.
static void hashCombine(size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// put Vertex into unordered_map to compare with self_define hash
struct VertexHash
{
    size_t operator()(const Vertex& v) const
    {
        size_t seed = 0;
        hashCombine(seed, std::hash<float>()(v.position.x));
        hashCombine(seed, std::hash<float>()(v.position.y));
        hashCombine(seed, std::hash<float>()(v.position.z));
        hashCombine(seed, std::hash<float>()(v.normal.x));
        hashCombine(seed, std::hash<float>()(v.normal.y));
        hashCombine(seed, std::hash<float>()(v.normal.z));
        hashCombine(seed, std::hash<float>()(v.uv.x));
        hashCombine(seed, std::hash<float>()(v.uv.y));
        return seed;
    }
};

struct VertexEqual
{
    bool operator()(const Vertex& a, const Vertex& b) const
    {
        return a.position == b.position && a.normal == b.normal && a.uv == b.uv;
    }
};

MeshData ObjLoader::load(const std::string& path)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    // tinyobj's mtl_basedir defaults to "" (not derived from path) when
    // omitted - without it, an OBJ with an mtllib directive (suzanne_lod1_uv.obj/
    // suzanne_lod2_uv.obj) has tinyobj look for the .mtl next to the
    // process's cwd (build/bin/) instead of next to the OBJ itself
    // (build/bin/assets/), so it never finds it - a harmless but noisy
    // "material file not found" warning, since materials here are never
    // read back (this codebase's actual materials come from the separate
    // Material class - see docs/TECHNICAL_NOTES.md §46/§25).
    std::string baseDir;
    size_t slashPos = path.find_last_of("/\\");
    if (slashPos != std::string::npos)
        baseDir = path.substr(0, slashPos + 1);

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str(), baseDir.c_str()))
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
            {  v.normal = { 0.0f, 0.0f, 1.0f };  }

            // read UV
            if (index.texcoord_index >= 0)
            {
                v.uv = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]  // Vulkan的V轴方向跟OBJ相反，要翻转
                };
            }
            else
            {  v.uv = { 0.0f, 0.0f };  }

            // 如果这个顶点没出现过，加进顶点数组，记下它的index
            if (uniqueVertices.count(v) == 0)
            {
                uniqueVertices[v] = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(v);
            }

            mesh.indices.push_back(uniqueVertices[v]);
        }
    }

    // Recenter to the mesh's own bounding-box center. Raw OBJ vertex
    // coordinates are authored around whatever origin the modeling tool
    // used (Suzanne's OBJ files are ~5 units off (0,0,0)) - the renderer's
    // per-instance model matrix only rotates (see FrameRenderer.cpp), so
    // an off-center mesh would visibly orbit its grid slot every frame
    // instead of spinning in place around it.
    glm::vec3 bboxMin(std::numeric_limits<float>::max());
    glm::vec3 bboxMax(std::numeric_limits<float>::lowest());
    for (const auto& v : mesh.vertices)
    {
        bboxMin = glm::min(bboxMin, v.position);
        bboxMax = glm::max(bboxMax, v.position);
    }
    glm::vec3 center = (bboxMin + bboxMax) * 0.5f;

    float radius = 0.0f;
    for (auto& v : mesh.vertices)
    {
        v.position -= center;
        radius = std::max(radius, glm::length(v.position));
    }
    mesh.boundingRadius = radius;

    std::cout << "[ObjLoader] " << mesh.vertices.size() << " unique vertices, "
               << mesh.indices.size() << " indices (from " << path << "), "
               << "recentered by (" << center.x << ", " << center.y << ", " << center.z
               << "), bounding radius " << radius << "\n";

    return mesh;
}

