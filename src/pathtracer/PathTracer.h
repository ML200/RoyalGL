#pragma once

#include <cstdint>
#include "gfx/Shader.h"
#include "gfx/Texture.h"
#include "gfx/Buffer.h"
#include "scene/Camera.h"
#include "scene/CameraSettings.h"
#include "bvh/BVHBuilder.h"
#include "optics/LensSystem.h"
#include "pathtracer/RenderSettings.h"

namespace RoyalGL
{
    // Owns the progressive-accumulation compute path tracer: the compute
    // shader program, the HDR accumulation image, and the per-frame uniform
    // buffer. Call Reset() whenever the camera, scene, settings, or lens
    // system change; otherwise call Render() once per frame to add one more
    // sample. `lensSystem` is only read when `cameraSettings.mode ==
    // CameraMode::LensSystem`; pass nullptr in pinhole mode.
    class PathTracer
    {
    public:
        PathTracer();

        void Resize(int width, int height);
        void Reset();
        void Render(const Camera& camera, const BVHBuilder& bvh, const RenderSettings& settings,
                    const CameraSettings& cameraSettings, const LensSystem* lensSystem);

        const Texture& AccumulationImage() const { return m_accum; }
        uint32_t SampleCount() const { return m_sampleCount; }
        int Width() const { return m_width; }
        int Height() const { return m_height; }

    private:
        Shader m_computeShader;
        Texture m_accum;
        Buffer m_frameUBO{BufferType::Uniform, 0};

        int m_width = 0;
        int m_height = 0;
        uint32_t m_sampleCount = 0;
    };
}
