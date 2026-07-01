#pragma once

#include <cstdint>
#include "scene/Scene.h"
#include "gfx/Buffer.h"

namespace RoyalGL
{
    // Builds a tinybvh BVH over the scene's world-space triangles and uploads
    // everything the path tracer's compute shader needs to trace against it:
    //   SSBO binding 1: BVH nodes      (tinybvh::BVH::BVHNode, Wald 32-byte layout)
    //   SSBO binding 2: triangle index (tinybvh primIdx permutation)
    //   SSBO binding 3: triangle data  (GPUTriangle, original scene order)
    //   SSBO binding 4: materials      (GPUMaterial)
    // See shaders/common.glsl for the matching GLSL struct declarations and
    // docs/ARCHITECTURE.md for why acceleration structure + scene geometry
    // storage are combined in one class for v1.
    class BVHBuilder
    {
    public:
        void Build(const Scene& scene);

        // Re-derives and re-uploads just the GPUMaterial[] SSBO, e.g. after
        // the user edits a material in the UI - skips the full BVH rebuild.
        void UpdateMaterials(const Scene& scene);

        uint32_t NodeCount() const { return m_nodeCount; }
        uint32_t TriangleCount() const { return m_triangleCount; }

        // Binds all four SSBOs to their configured binding points.
        void BindAll() const;

    private:
        Buffer m_nodeBuffer{BufferType::ShaderStorage, 1};
        Buffer m_indexBuffer{BufferType::ShaderStorage, 2};
        Buffer m_triBuffer{BufferType::ShaderStorage, 3};
        Buffer m_materialBuffer{BufferType::ShaderStorage, 4};

        uint32_t m_nodeCount = 0;
        uint32_t m_triangleCount = 0;
    };
}
