#pragma once

#include <cstdint>
#include <future>
#include <vector>
#include "scene/Scene.h"
#include "gfx/Buffer.h"

namespace RoyalGL
{
    // Mirror of tinybvh::BVH::BVHNode (Wald 32-byte layout) and of the GLSL
    // BVHNode in shaders/common.glsl, so BLAS blocks can be cached and
    // stitched on the CPU without exposing tiny_bvh.h outside its single
    // implementation TU.
    struct BVHFlatNode
    {
        glm::vec3 bmin;
        uint32_t leftFirst;
        glm::vec3 bmax;
        uint32_t triCount;
    };
    static_assert(sizeof(BVHFlatNode) == 32, "must match tinybvh/GLSL BVHNode");

    // Two-level acceleration structure over the scene's world-space
    // triangles, flattened into the single-level Wald BVH2 layout the GPU
    // traversal already consumes (zero shader changes):
    //   - one BLAS per SceneInstance, built with tinybvh and CACHED - moving
    //     an instance rebuilds only that instance's BLAS;
    //   - a small TLAS over the instance AABBs, rebuilt on every change (it
    //     has 2N-1 nodes for N instances - a refit would save nothing);
    //   - the flatten step stitches TLAS + BLAS blocks into one node array:
    //     TLAS leaves are COPIES of the BLAS root nodes with their child /
    //     triangle-index pointers offset into the appended blocks, which
    //     preserves the "right child = left child + 1" adjacency the GPU
    //     traversal assumes.
    // Instance moves are rebuilt ASYNCHRONOUSLY (the build is CPU-side):
    // RequestInstanceRebuild snapshots the inputs and runs transform + BLAS
    // + TLAS + flatten on a worker thread; PumpAsync (main thread, owns the
    // GL context) applies the finished result - writes the retransformed
    // triangles into the scene and re-uploads the SSBOs. Rendering keeps
    // using the previous consistent BVH/triangle pair until then.
    //
    // Uploads (see shaders/common.glsl):
    //   SSBO binding 1: BVH nodes      (flattened TLAS + BLAS blocks)
    //   SSBO binding 2: triangle index (per-BLAS primIdx, globally offset)
    //   SSBO binding 3: triangle data  (GPUTriangle, original scene order)
    //   SSBO binding 4: materials      (GPUMaterial)
    class BVHBuilder
    {
    public:
        // Synchronous full build (startup / scene load): every BLAS + TLAS.
        void Build(const Scene& scene);

        // Re-derives and re-uploads just the GPUMaterial[] SSBO, e.g. after
        // the user edits a material in the UI - skips the full BVH rebuild.
        void UpdateMaterials(const Scene& scene);

        // Kicks the async rebuild of one moved instance (its BLAS + the
        // TLAS + flatten). Returns false if a worker is already running -
        // the caller keeps its dirty flag and retries after PumpAsync.
        bool RequestInstanceRebuild(const Scene& scene, size_t instanceIndex);

        // Main-thread poll: when the worker finished, writes the instance's
        // retransformed triangles into scene.triangles, uploads all
        // geometry buffers, and returns true (caller then rebuilds the
        // light tree and resets accumulation).
        bool PumpAsync(Scene& scene);
        bool AsyncBusy() const { return m_jobRunning; }

        uint32_t NodeCount() const { return m_nodeCount; }
        uint32_t TriangleCount() const { return m_triangleCount; }

        // The transform each instance's UPLOADED geometry currently uses
        // (lags the UI value while an async rebuild is in flight) and the
        // instances' first-triangle table - PathTracer uploads these so
        // shaders can store frame-persistent surface data in instance
        // OBJECT SPACE (moving objects then carry their reservoir /
        // G-buffer anchors along instead of ghosting).
        const std::vector<glm::mat4>& EffectiveInstanceMatrices() const { return m_effectiveMatrix; }
        const std::vector<uint32_t>& InstanceFirstTriangles() const { return m_instanceFirstTri; }

        // Binds all four SSBOs to their configured binding points.
        void BindAll() const;

    private:
        struct Blas
        {
            std::vector<BVHFlatNode> nodes;
            std::vector<uint32_t> indices; // global triangle ids
        };
        struct BuildOutput
        {
            size_t instanceIndex = 0;
            glm::mat4 matrix{1.0f}; // the transform this rebuild applied
            std::vector<Triangle> worldTriangles;
            Blas blas;
            std::vector<BVHFlatNode> nodes;  // flattened TLAS + BLAS blocks
            std::vector<uint32_t> indices;
        };

        void UploadFlattened(const std::vector<BVHFlatNode>& nodes, const std::vector<uint32_t>& indices);
        void UploadTriangles(const Scene& scene);

        Buffer m_nodeBuffer{BufferType::ShaderStorage, 1};
        Buffer m_indexBuffer{BufferType::ShaderStorage, 2};
        Buffer m_triBuffer{BufferType::ShaderStorage, 3};
        Buffer m_materialBuffer{BufferType::ShaderStorage, 4};

        std::vector<Blas> m_blasCache; // one per SceneInstance
        std::vector<glm::mat4> m_effectiveMatrix;
        std::vector<uint32_t> m_instanceFirstTri;
        std::future<BuildOutput> m_job;
        bool m_jobRunning = false;

        uint32_t m_nodeCount = 0;
        uint32_t m_triangleCount = 0;
    };
}
