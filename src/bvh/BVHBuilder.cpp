#include "bvh/BVHBuilder.h"
#include "core/Log.h"

#include <GL/glew.h>

#if defined(__MINGW32__) || defined(__MINGW64__)
// tiny_bvh.h's default aligned-allocation macros for "GCC on non-Linux"
// (i.e. MinGW) allocate with _mm_malloc but free with plain free() - a
// mismatched pairing that corrupts the heap the first time a BVH is
// destroyed. Override both macros (tiny_bvh.h honors a prior definition via
// its own #ifndef _ALIGNED_ALLOC guard) with a correctly-paired
// _aligned_malloc/_aligned_free, which MinGW-w64 provides for MSVC CRT
// compatibility. ALIGNED(x) must be redefined too since it normally lives
// behind the same guard. make_multiple_of() is declared by tiny_bvh.h itself
// above this guard, so it is in scope at the point these macros expand.
#include <malloc.h>
#define ALIGNED(x) __attribute__((aligned(x)))
#define _ALIGNED_ALLOC(alignment, size) _aligned_malloc(make_multiple_of(size, alignment), alignment)
#define _ALIGNED_FREE(ptr) _aligned_free(ptr)
#endif

// This is the sole translation unit that instantiates tinybvh's
// implementation (single-header, stb-style library).
#define TINYBVH_IMPLEMENTATION
#include <tiny_bvh.h>

#include <vector>

namespace RoyalGL
{
    void BVHBuilder::Build(const Scene& scene)
    {
        const size_t triCount = scene.triangles.size();
        if (triCount == 0)
        {
            ROYALGL_LOG_WARN("BVHBuilder: scene has no triangles, nothing to build.");
            m_nodeCount = 0;
            m_triangleCount = 0;
            return;
        }

        // tinybvh consumes a flat, non-indexed vertex soup: 3 vertices per
        // triangle, contiguous. The 4th float per vertex is unused padding
        // (see tiny_bvh.h's bvhvec4 documentation).
        std::vector<tinybvh::bvhvec4> verts;
        verts.reserve(triCount * 3);
        for (const Triangle& tri : scene.triangles)
        {
            verts.emplace_back(tri.v0.position.x, tri.v0.position.y, tri.v0.position.z, 0.0f);
            verts.emplace_back(tri.v1.position.x, tri.v1.position.y, tri.v1.position.z, 0.0f);
            verts.emplace_back(tri.v2.position.x, tri.v2.position.y, tri.v2.position.z, 0.0f);
        }

        tinybvh::BVH bvh;
        bvh.Build(verts.data(), static_cast<uint32_t>(triCount));

        m_nodeCount = bvh.usedNodes;
        m_triangleCount = static_cast<uint32_t>(triCount);

        // tinybvh::BVH::BVHNode is the "Wald" 32-byte layout (vec3+uint,
        // vec3+uint) - its C++ byte layout already matches the std430 layout
        // of the mirrored `BVHNode` struct in shaders/common.glsl, so it is
        // uploaded completely unmodified.
        m_nodeBuffer.Upload(bvh.bvhNode,
                             static_cast<size_t>(bvh.usedNodes) * sizeof(tinybvh::BVH::BVHNode),
                             GL_STATIC_DRAW);

        // bvh.primIdx is the permutation from BVH leaf slot -> original
        // triangle index; triangles themselves are never reordered.
        m_indexBuffer.Upload(bvh.primIdx,
                              static_cast<size_t>(bvh.idxCount) * sizeof(uint32_t),
                              GL_STATIC_DRAW);

        std::vector<GPUTriangle> gpuTriangles = scene.BuildGPUTriangles();
        m_triBuffer.Upload(gpuTriangles.data(), gpuTriangles.size() * sizeof(GPUTriangle), GL_STATIC_DRAW);

        std::vector<GPUMaterial> gpuMaterials = scene.BuildGPUMaterials();
        m_materialBuffer.Upload(gpuMaterials.data(), gpuMaterials.size() * sizeof(GPUMaterial), GL_STATIC_DRAW);

        ROYALGL_LOG_INFO("BVHBuilder: built BVH over ", triCount, " triangles (", bvh.usedNodes, " nodes, ",
                          gpuMaterials.size(), " materials).");
    }

    void BVHBuilder::UpdateMaterials(const Scene& scene)
    {
        std::vector<GPUMaterial> gpuMaterials = scene.BuildGPUMaterials();
        m_materialBuffer.Upload(gpuMaterials.data(), gpuMaterials.size() * sizeof(GPUMaterial), GL_STATIC_DRAW);
    }

    void BVHBuilder::BindAll() const
    {
        m_nodeBuffer.BindBase();
        m_indexBuffer.BindBase();
        m_triBuffer.BindBase();
        m_materialBuffer.BindBase();
    }
}
