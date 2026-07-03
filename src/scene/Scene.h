#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/Mesh.h"
#include "gfx/GPUTypes.h"

namespace RoyalGL
{
    // A movable group of triangles: a contiguous range of Scene::triangles
    // plus the world-space rest pose captured at registration and a
    // user-editable TRS relative to it. The world triangles are re-derived
    // from the rest pose whenever the transform changes (rest -> world, so
    // repeated edits never accumulate error). The transform pivot is the
    // rest bounds center.
    struct SceneInstance
    {
        std::string name;
        uint32_t firstTriangle = 0;
        uint32_t triangleCount = 0;
        std::vector<Triangle> restTriangles;
        glm::vec3 pivot{0.0f};

        glm::vec3 position{0.0f};    // translation from the rest pose
        glm::vec3 rotationDeg{0.0f}; // XYZ Euler, applied about the pivot
        float scale = 1.0f;          // uniform, about the pivot

        bool IsIdentity() const
        {
            return position == glm::vec3(0.0f) && rotationDeg == glm::vec3(0.0f) && scale == 1.0f;
        }
        glm::mat4 Matrix() const;

        // rest -> world under the current TRS; normals are rotated (uniform
        // scale + translation leave them unchanged otherwise).
        std::vector<Triangle> TransformedTriangles() const;
    };

    // A flattened, GPU-ready description of everything the path tracer needs:
    // world-space triangles (already transformed), their materials, and a
    // camera. glTF scene-graph hierarchy is resolved once at load time in
    // GLTFLoader; nothing downstream needs to know about scene-graph nodes.
    // Triangles are grouped into instances (see SceneInstance) so objects
    // can be moved after load; instances tile the triangle array exactly.
    class Scene
    {
    public:
        std::vector<Triangle> triangles;
        std::vector<Material> materials;
        std::vector<SceneInstance> instances;
        Camera camera;
        std::string sourcePath;

        // A small Cornell-box-style scene (floor/walls/ceiling, one emissive
        // "light" quad, one box) used when no .glb is provided or loading
        // fails, so the app always has something interesting to path trace.
        void LoadFallbackScene();

        // Appends another scene's triangles as one "instance": uniformly
        // scaled so its height becomes `targetHeight`, translated so its
        // bounds' bottom center lands on `floorCenter`, and with every
        // triangle's material replaced by `material` (appended to this
        // scene's material list). Registers the range as a movable instance.
        void MergeInstance(const Scene& other, const glm::vec3& floorCenter, float targetHeight,
                           const Material& material, const std::string& name = "Instance");

        // Captures triangles [firstTriangle, triangles.size()) as one movable
        // instance with the current world positions as its rest pose.
        void RegisterInstance(const std::string& name, uint32_t firstTriangle);

        // Writes instance i's transformed rest pose back into the shared
        // triangle array (used by the synchronous path; the async BVH worker
        // computes the same via SceneInstance::TransformedTriangles).
        void ApplyInstanceTransform(size_t index);

        glm::vec3 BoundsMin() const;
        glm::vec3 BoundsMax() const;

        // Pure data transforms - no GL calls - so BVHBuilder can upload the
        // results without Scene needing to know anything about OpenGL.
        std::vector<GPUTriangle> BuildGPUTriangles() const;
        std::vector<GPUMaterial> BuildGPUMaterials() const;
    };
}
