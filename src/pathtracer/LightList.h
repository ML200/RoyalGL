#pragma once

#include <cstdint>
#include "scene/Scene.h"
#include "gfx/Buffer.h"

namespace RoyalGL
{
    // Scans Scene::triangles for triangles whose material has non-zero
    // emissive radiance and uploads a compact SSBO describing them as
    // sampleable area lights, for use by the flare/ghost light-tracing
    // pass only (pathtracer/LensFlare.h) - the main path tracer still finds
    // emissive triangles opportunistically via BSDF-sampled hits and does
    // not consume this list.
    class LightList
    {
    public:
        void Build(const Scene& scene);

        uint32_t LightCount() const { return m_lightCount; }

        // Binds the light SSBO (binding 6) and the sampling CDF SSBO (binding 7).
        void BindAll() const;

    private:
        Buffer m_lightBuffer{BufferType::ShaderStorage, 6};
        Buffer m_cdfBuffer{BufferType::ShaderStorage, 7};
        uint32_t m_lightCount = 0;
    };
}
