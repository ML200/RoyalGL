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
    // A flattened, GPU-ready description of everything the path tracer needs:
    // world-space triangles (already transformed), their materials, and a
    // camera. glTF scene-graph hierarchy is resolved once at load time in
    // GLTFLoader; nothing downstream needs to know about scene-graph nodes.
    class Scene
    {
    public:
        std::vector<Triangle> triangles;
        std::vector<Material> materials;
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
        // scene's material list).
        void MergeInstance(const Scene& other, const glm::vec3& floorCenter, float targetHeight,
                           const Material& material);

        glm::vec3 BoundsMin() const;
        glm::vec3 BoundsMax() const;

        // Pure data transforms - no GL calls - so BVHBuilder can upload the
        // results without Scene needing to know anything about OpenGL.
        std::vector<GPUTriangle> BuildGPUTriangles() const;
        std::vector<GPUMaterial> BuildGPUMaterials() const;
    };
}
