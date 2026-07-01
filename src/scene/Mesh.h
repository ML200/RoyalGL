#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace RoyalGL
{
    // One vertex of a flattened scene triangle, already in world space.
    struct Vertex
    {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::vec2 uv{0.0f};
    };

    // A single world-space triangle plus the index of its material in
    // Scene::materials. This is the CPU-side unit both tinybvh and the GPU
    // triangle buffer are built from - there is no separate glTF-mesh
    // abstraction downstream of GLTFLoader (see docs/ARCHITECTURE.md).
    struct Triangle
    {
        Vertex v0, v1, v2;
        uint32_t materialIndex = 0;
    };
}
