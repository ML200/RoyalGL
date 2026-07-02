#pragma once

#include <cstdint>
#include "scene/Scene.h"
#include "gfx/Buffer.h"

namespace RoyalGL
{
    // 4-wide light BVH over the scene's emissive triangles, used by the path
    // tracer's next-event-estimation pass (see shaders/light_tree.glsl).
    // Ported from RoyalTracer-DX's LightTree.h: same SAOH (surface area +
    // orientation) split heuristic, normal-cone merging and GPU node layout,
    // collapsed from that engine's two-level TLAS/BLAS to a single level
    // because RoyalGL's scene is one flattened world-space triangle soup
    // with no instances.
    //
    // Rebuild whenever scene geometry or any material's emissive value
    // changes (an emissive edit re-weights or adds/removes tree leaves).
    class LightTree
    {
    public:
        // CPU build + GPU upload in one step. Safe to call repeatedly.
        void Build(const Scene& scene);

        // Binds the node SSBO (binding 5), the light-triangle SSBO
        // (binding 6), the sceneTri->lightTri map SSBO (binding 7) and the
        // power CDF SSBO (binding 10, used by the bidirectional kernels for
        // receiver-independent light selection).
        void BindAll() const;

        uint32_t LightCount() const { return m_lightCount; }
        float TotalPower() const { return m_totalPower; }

    private:
        Buffer m_nodeBuffer{BufferType::ShaderStorage, 5};
        Buffer m_triBuffer{BufferType::ShaderStorage, 6};
        Buffer m_mapBuffer{BufferType::ShaderStorage, 7};
        Buffer m_cdfBuffer{BufferType::ShaderStorage, 10};
        uint32_t m_lightCount = 0;
        float m_totalPower = 0.0f;
    };
}
