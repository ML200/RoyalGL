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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <vector>

namespace RoyalGL
{
    namespace
    {
        struct BlasData
        {
            std::vector<BVHFlatNode> nodes;
            std::vector<uint32_t> indices;
        };

        // Builds one BLAS over `count` world-space triangles whose global
        // ids start at `globalFirst`. CPU-only (worker-thread safe).
        BlasData BuildBlas(const Triangle* tris, uint32_t count, uint32_t globalFirst)
        {
            BlasData out;
            if (count == 0) return out;

            std::vector<tinybvh::bvhvec4> verts;
            verts.reserve(size_t(count) * 3);
            for (uint32_t i = 0; i < count; ++i)
            {
                const Triangle& tri = tris[i];
                verts.emplace_back(tri.v0.position.x, tri.v0.position.y, tri.v0.position.z, 0.0f);
                verts.emplace_back(tri.v1.position.x, tri.v1.position.y, tri.v1.position.z, 0.0f);
                verts.emplace_back(tri.v2.position.x, tri.v2.position.y, tri.v2.position.z, 0.0f);
            }

            tinybvh::BVH bvh;
            bvh.Build(verts.data(), count);

            out.nodes.resize(bvh.usedNodes);
            static_assert(sizeof(BVHFlatNode) == sizeof(tinybvh::BVH::BVHNode),
                          "BVHFlatNode must mirror tinybvh's node");
            std::memcpy(out.nodes.data(), bvh.bvhNode, size_t(bvh.usedNodes) * sizeof(BVHFlatNode));

            out.indices.resize(bvh.idxCount);
            for (uint32_t i = 0; i < bvh.idxCount; ++i)
                out.indices[i] = bvh.primIdx[i] + globalFirst;
            return out;
        }

        // Stitches the cached BLAS blocks under a freshly built TLAS into
        // one node array in the exact layout the GPU traversal consumes:
        // root at node 0, every internal node's children adjacent
        // (leftFirst, leftFirst+1). TLAS leaves are COPIES of the BLAS root
        // nodes with their pointers offset into the appended blocks; the
        // adjacency inside each block is preserved by construction, so a
        // uniform per-block offset is the only fixup needed. CPU-only.
        void FlattenTlas(const std::vector<std::pair<std::vector<BVHFlatNode> const*,
                                                     std::vector<uint32_t> const*>>& blases,
                         std::vector<BVHFlatNode>& outNodes, std::vector<uint32_t>& outIndices)
        {
            outNodes.clear();
            outIndices.clear();
            size_t n = blases.size();
            if (n == 0) return;

            // Per-block node / index offsets in the final arrays.
            size_t tlasNodes = (n == 1) ? 0 : (2 * n - 1);
            std::vector<uint32_t> nodeOffset(n), indexOffset(n);
            uint32_t nodeCursor = static_cast<uint32_t>(tlasNodes);
            uint32_t indexCursor = 0;
            for (size_t i = 0; i < n; ++i)
            {
                nodeOffset[i] = nodeCursor;
                indexOffset[i] = indexCursor;
                nodeCursor += static_cast<uint32_t>(blases[i].first->size());
                indexCursor += static_cast<uint32_t>(blases[i].second->size());
            }

            outNodes.resize(nodeCursor);
            outIndices.resize(indexCursor);

            // Copy the BLAS blocks with pointer fixups.
            for (size_t i = 0; i < n; ++i)
            {
                const std::vector<BVHFlatNode>& src = *blases[i].first;
                for (size_t k = 0; k < src.size(); ++k)
                {
                    BVHFlatNode node = src[k];
                    node.leftFirst += (node.triCount > 0) ? indexOffset[i] : nodeOffset[i];
                    outNodes[nodeOffset[i] + k] = node;
                }
                std::copy(blases[i].second->begin(), blases[i].second->end(),
                          outIndices.begin() + indexOffset[i]);
            }

            // A single instance needs no TLAS: its BLAS root already sits at
            // node 0 and the offsets above were zero.
            if (n == 1) return;

            // Root copy of BLAS i, pointers offset into its block.
            auto blasRootCopy = [&](size_t i)
            {
                BVHFlatNode node = (*blases[i].first)[0];
                node.leftFirst += (node.triCount > 0) ? indexOffset[i] : nodeOffset[i];
                return node;
            };

            // Recursive TLAS emit: `slot` receives either a BLAS root copy
            // (single item) or an internal node whose children go into a
            // freshly allocated adjacent pair. Median split on the longest
            // centroid axis - instance counts are tiny.
            struct Item { size_t inst; glm::vec3 centroid; };
            std::vector<Item> items;
            items.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                const BVHFlatNode& root = (*blases[i].first)[0];
                items.push_back({i, 0.5f * (root.bmin + root.bmax)});
            }

            uint32_t pairCursor = 1; // node 0 = TLAS root; pairs fill 1..2n-2
            // NOLINTNEXTLINE(misc-no-recursion)
            std::function<void(uint32_t, std::vector<Item>&)> emit =
                [&](uint32_t slot, std::vector<Item>& span)
            {
                if (span.size() == 1)
                {
                    outNodes[slot] = blasRootCopy(span[0].inst);
                    return;
                }
                glm::vec3 cmin = span[0].centroid, cmax = span[0].centroid;
                for (const Item& it : span)
                {
                    cmin = glm::min(cmin, it.centroid);
                    cmax = glm::max(cmax, it.centroid);
                }
                glm::vec3 ext = cmax - cmin;
                int axis = (ext.y > ext.x) ? ((ext.z > ext.y) ? 2 : 1) : ((ext.z > ext.x) ? 2 : 0);
                std::sort(span.begin(), span.end(),
                          [axis](const Item& a, const Item& b) { return a.centroid[axis] < b.centroid[axis]; });
                size_t mid = span.size() / 2;
                std::vector<Item> left(span.begin(), span.begin() + mid);
                std::vector<Item> right(span.begin() + mid, span.end());

                uint32_t pair = pairCursor;
                pairCursor += 2;
                emit(pair, left);
                emit(pair + 1, right);

                BVHFlatNode node;
                node.bmin = glm::min(outNodes[pair].bmin, outNodes[pair + 1].bmin);
                node.bmax = glm::max(outNodes[pair].bmax, outNodes[pair + 1].bmax);
                node.leftFirst = pair;
                node.triCount = 0;
                outNodes[slot] = node;
            };
            emit(0, items);
        }
    }

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

        // One BLAS per instance. If the instances don't exactly tile the
        // triangle array (defensive - registration should guarantee it),
        // fall back to a single implicit instance over everything.
        m_blasCache.clear();
        bool tiled = !scene.instances.empty();
        uint32_t cursor = 0;
        for (const SceneInstance& inst : scene.instances)
        {
            if (inst.firstTriangle != cursor) { tiled = false; break; }
            cursor += inst.triangleCount;
        }
        if (tiled && cursor != triCount) tiled = false;

        m_effectiveMatrix.clear();
        m_instanceFirstTri.clear();
        if (tiled)
        {
            for (const SceneInstance& inst : scene.instances)
            {
                BlasData b = BuildBlas(scene.triangles.data() + inst.firstTriangle,
                                       inst.triangleCount, inst.firstTriangle);
                m_blasCache.push_back({std::move(b.nodes), std::move(b.indices)});
                m_effectiveMatrix.push_back(inst.Matrix());
                m_instanceFirstTri.push_back(inst.firstTriangle);
            }
        }
        else
        {
            BlasData b = BuildBlas(scene.triangles.data(), static_cast<uint32_t>(triCount), 0);
            m_blasCache.push_back({std::move(b.nodes), std::move(b.indices)});
            m_effectiveMatrix.push_back(glm::mat4(1.0f));
            m_instanceFirstTri.push_back(0);
        }

        std::vector<std::pair<std::vector<BVHFlatNode> const*, std::vector<uint32_t> const*>> refs;
        refs.reserve(m_blasCache.size());
        for (const Blas& b : m_blasCache)
            refs.push_back({&b.nodes, &b.indices});

        std::vector<BVHFlatNode> nodes;
        std::vector<uint32_t> indices;
        FlattenTlas(refs, nodes, indices);

        m_triangleCount = static_cast<uint32_t>(triCount);
        UploadFlattened(nodes, indices);
        UploadTriangles(scene);

        std::vector<GPUMaterial> gpuMaterials = scene.BuildGPUMaterials();
        m_materialBuffer.Upload(gpuMaterials.data(), gpuMaterials.size() * sizeof(GPUMaterial), GL_STATIC_DRAW);

        ROYALGL_LOG_INFO("BVHBuilder: built ", m_blasCache.size(), " BLAS(es) + TLAS over ", triCount,
                          " triangles (", m_nodeCount, " nodes, ", gpuMaterials.size(), " materials).");
    }

    bool BVHBuilder::RequestInstanceRebuild(const Scene& scene, size_t instanceIndex)
    {
        if (m_jobRunning) return false;
        if (instanceIndex >= scene.instances.size() ||
            scene.instances.size() != m_blasCache.size())
            return false;

        // Snapshot everything the worker touches: the instance (rest pose +
        // transform, copied) and the other instances' cached BLAS blocks
        // (copied - cheap next to a build, and it makes the worker fully
        // self-contained: no shared mutable state, no GL).
        SceneInstance inst = scene.instances[instanceIndex];
        std::vector<Blas> cache = m_blasCache;

        m_jobRunning = true;
        m_job = std::async(std::launch::async,
            [instanceIndex, inst = std::move(inst), cache = std::move(cache)]() mutable
            {
                BuildOutput out;
                out.instanceIndex = instanceIndex;
                out.matrix = inst.Matrix();
                out.worldTriangles = inst.TransformedTriangles();
                BlasData b = BuildBlas(out.worldTriangles.data(), inst.triangleCount, inst.firstTriangle);
                out.blas = {std::move(b.nodes), std::move(b.indices)};
                cache[instanceIndex] = out.blas;

                std::vector<std::pair<std::vector<BVHFlatNode> const*, std::vector<uint32_t> const*>> refs;
                refs.reserve(cache.size());
                for (const Blas& blas : cache)
                    refs.push_back({&blas.nodes, &blas.indices});
                FlattenTlas(refs, out.nodes, out.indices);
                return out;
            });
        return true;
    }

    bool BVHBuilder::PumpAsync(Scene& scene)
    {
        if (!m_jobRunning) return false;
        if (m_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;

        BuildOutput out = m_job.get();
        m_jobRunning = false;

        m_blasCache[out.instanceIndex] = std::move(out.blas);
        if (out.instanceIndex < m_effectiveMatrix.size())
            m_effectiveMatrix[out.instanceIndex] = out.matrix;

        // Apply the retransformed triangles and re-upload - main thread,
        // owns the GL context. The scene + BVH + triangle buffer stay a
        // consistent set at every point in time.
        const SceneInstance& inst = scene.instances[out.instanceIndex];
        std::copy(out.worldTriangles.begin(), out.worldTriangles.end(),
                  scene.triangles.begin() + inst.firstTriangle);

        UploadFlattened(out.nodes, out.indices);
        UploadTriangles(scene);
        return true;
    }

    void BVHBuilder::UploadFlattened(const std::vector<BVHFlatNode>& nodes, const std::vector<uint32_t>& indices)
    {
        m_nodeCount = static_cast<uint32_t>(nodes.size());
        m_nodeBuffer.Upload(nodes.data(), nodes.size() * sizeof(BVHFlatNode), GL_STATIC_DRAW);
        m_indexBuffer.Upload(indices.data(), indices.size() * sizeof(uint32_t), GL_STATIC_DRAW);
    }

    void BVHBuilder::UploadTriangles(const Scene& scene)
    {
        std::vector<GPUTriangle> gpuTriangles = scene.BuildGPUTriangles();
        m_triBuffer.Upload(gpuTriangles.data(), gpuTriangles.size() * sizeof(GPUTriangle), GL_STATIC_DRAW);
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
