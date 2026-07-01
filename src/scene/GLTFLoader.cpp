#include "scene/GLTFLoader.h"
#include "core/Log.h"

// This is the sole translation unit that instantiates cgltf's implementation
// (single-header, stb-style library).
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cfloat>
#include <vector>

namespace RoyalGL
{
    namespace
    {
        glm::mat4 NodeWorldMatrix(const cgltf_node* node)
        {
            float m[16];
            cgltf_node_transform_world(node, m);
            return glm::make_mat4(m);
        }

        Material ConvertMaterial(const cgltf_material* mat)
        {
            Material out;
            if (mat && mat->has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
                out.baseColor = glm::vec3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
                out.metallic = pbr.metallic_factor;
                out.roughness = pbr.roughness_factor;
            }
            if (mat)
            {
                float strength = mat->has_emissive_strength ? mat->emissive_strength.emissive_strength : 1.0f;
                out.emissive = glm::vec3(mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2]) * strength;
            }
            return out;
        }

        // Reads a whole accessor into a flat float buffer, `numComponents`
        // floats per element (handles normalized integer attributes correctly).
        std::vector<float> ReadAccessorFloats(const cgltf_accessor* accessor, cgltf_size numComponents)
        {
            std::vector<float> data(accessor->count * numComponents);
            cgltf_accessor_unpack_floats(accessor, data.data(), data.size());
            return data;
        }

        void AppendPrimitive(const cgltf_primitive& prim, const glm::mat4& worldMatrix,
                              uint32_t materialIndex, std::vector<Triangle>& outTriangles)
        {
            if (prim.type != cgltf_primitive_type_triangles) return;

            const cgltf_accessor* posAccessor = nullptr;
            const cgltf_accessor* normalAccessor = nullptr;
            const cgltf_accessor* uvAccessor = nullptr;

            for (cgltf_size i = 0; i < prim.attributes_count; i++)
            {
                const cgltf_attribute& attr = prim.attributes[i];
                if (attr.type == cgltf_attribute_type_position) posAccessor = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) normalAccessor = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvAccessor = attr.data;
            }
            if (!posAccessor) return;

            std::vector<float> positions = ReadAccessorFloats(posAccessor, 3);
            std::vector<float> normals = normalAccessor ? ReadAccessorFloats(normalAccessor, 3) : std::vector<float>();
            std::vector<float> uvs = uvAccessor ? ReadAccessorFloats(uvAccessor, 2) : std::vector<float>();

            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldMatrix)));

            auto makeVertex = [&](cgltf_size vertexIndex) -> Vertex
            {
                Vertex v;
                glm::vec4 localPos(positions[vertexIndex * 3 + 0], positions[vertexIndex * 3 + 1], positions[vertexIndex * 3 + 2], 1.0f);
                v.position = glm::vec3(worldMatrix * localPos);
                if (!normals.empty())
                {
                    glm::vec3 n(normals[vertexIndex * 3 + 0], normals[vertexIndex * 3 + 1], normals[vertexIndex * 3 + 2]);
                    v.normal = glm::normalize(normalMatrix * n);
                }
                if (!uvs.empty())
                    v.uv = glm::vec2(uvs[vertexIndex * 2 + 0], uvs[vertexIndex * 2 + 1]);
                return v;
            };

            std::vector<uint32_t> indices;
            if (prim.indices)
            {
                indices.resize(prim.indices->count);
                for (cgltf_size i = 0; i < prim.indices->count; i++)
                    indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
            }
            else
            {
                indices.resize(posAccessor->count);
                for (cgltf_size i = 0; i < posAccessor->count; i++)
                    indices[i] = static_cast<uint32_t>(i);
            }

            bool haveNormals = !normals.empty();

            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                Triangle tri;
                tri.v0 = makeVertex(indices[i + 0]);
                tri.v1 = makeVertex(indices[i + 1]);
                tri.v2 = makeVertex(indices[i + 2]);
                tri.materialIndex = materialIndex;

                if (!haveNormals)
                {
                    glm::vec3 faceNormal = glm::normalize(glm::cross(tri.v1.position - tri.v0.position, tri.v2.position - tri.v0.position));
                    tri.v0.normal = tri.v1.normal = tri.v2.normal = faceNormal;
                }

                outTriangles.push_back(tri);
            }
        }

        void VisitNode(const cgltf_node* node, const cgltf_data* data, std::vector<Triangle>& outTriangles)
        {
            if (node->mesh)
            {
                glm::mat4 worldMatrix = NodeWorldMatrix(node);
                for (cgltf_size i = 0; i < node->mesh->primitives_count; i++)
                {
                    const cgltf_primitive& prim = node->mesh->primitives[i];
                    uint32_t materialIndex = 0;
                    if (prim.material)
                    {
                        ptrdiff_t idx = prim.material - data->materials;
                        if (idx >= 0 && static_cast<cgltf_size>(idx) < data->materials_count)
                            materialIndex = static_cast<uint32_t>(idx);
                    }
                    AppendPrimitive(prim, worldMatrix, materialIndex, outTriangles);
                }
            }
            for (cgltf_size i = 0; i < node->children_count; i++)
                VisitNode(node->children[i], data, outTriangles);
        }
    }

    bool GLTFLoader::Load(const std::filesystem::path& path, Scene& outScene)
    {
        cgltf_options options{};
        cgltf_data* data = nullptr;

        std::string pathStr = path.string();
        cgltf_result result = cgltf_parse_file(&options, pathStr.c_str(), &data);
        if (result != cgltf_result_success)
        {
            ROYALGL_LOG_ERROR("GLTFLoader: failed to parse '", pathStr, "' (cgltf error ", static_cast<int>(result), ")");
            return false;
        }

        result = cgltf_load_buffers(&options, data, pathStr.c_str());
        if (result != cgltf_result_success)
        {
            ROYALGL_LOG_ERROR("GLTFLoader: failed to load buffers for '", pathStr, "' (cgltf error ", static_cast<int>(result), ")");
            cgltf_free(data);
            return false;
        }

        if (cgltf_validate(data) != cgltf_result_success)
            ROYALGL_LOG_WARN("GLTFLoader: '", pathStr, "' failed validation, attempting to load anyway.");

        Scene scene;
        scene.sourcePath = pathStr;

        scene.materials.reserve(data->materials_count);
        for (cgltf_size i = 0; i < data->materials_count; i++)
            scene.materials.push_back(ConvertMaterial(&data->materials[i]));
        if (scene.materials.empty())
            scene.materials.push_back(Material{});

        const cgltf_scene* gltfScene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
        if (gltfScene)
        {
            for (cgltf_size i = 0; i < gltfScene->nodes_count; i++)
                VisitNode(gltfScene->nodes[i], data, scene.triangles);
        }
        else
        {
            for (cgltf_size i = 0; i < data->nodes_count; i++)
                if (!data->nodes[i].parent)
                    VisitNode(&data->nodes[i], data, scene.triangles);
        }

        cgltf_free(data);

        if (scene.triangles.empty())
        {
            ROYALGL_LOG_ERROR("GLTFLoader: '", pathStr, "' produced zero triangles.");
            return false;
        }

        // Frame a default orbit camera around the loaded geometry (glTF
        // camera nodes are not imported in v1 - see docs/ARCHITECTURE.md).
        glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);
        for (const Triangle& tri : scene.triangles)
        {
            for (const Vertex* v : {&tri.v0, &tri.v1, &tri.v2})
            {
                boundsMin = glm::min(boundsMin, v->position);
                boundsMax = glm::max(boundsMax, v->position);
            }
        }
        glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
        float radius = glm::max(glm::length(boundsMax - boundsMin) * 0.5f, 0.1f);

        scene.camera.target = center;
        scene.camera.position = center + glm::vec3(0.4f, 0.35f, 1.0f) * radius * 2.2f;

        outScene = std::move(scene);
        ROYALGL_LOG_INFO("GLTFLoader: loaded '", pathStr, "' - ", outScene.triangles.size(), " triangles, ",
                          outScene.materials.size(), " materials.");
        return true;
    }
}
