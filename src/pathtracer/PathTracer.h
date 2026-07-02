#pragma once

#include <cstdint>
#include "gfx/Shader.h"
#include "gfx/Texture.h"
#include "gfx/Buffer.h"
#include "scene/Camera.h"
#include "bvh/BVHBuilder.h"
#include "optics/LensSystem.h"
#include "pathtracer/LightTree.h"
#include "pathtracer/RenderSettings.h"

namespace RoyalGL
{
    // Owns the progressive-accumulation path tracer: the compute shader
    // programs, the HDR accumulation image, and the per-frame uniform
    // buffer. Two pipelines share the accumulation image, selected by
    // RenderSettings::enableBidir per frame:
    //  - unidirectional: one megakernel (pathtrace.comp) with light-tree
    //    NEE + MIS,
    //  - bidirectional: three passes (light subpaths -> eye subpaths with
    //    vertex connections -> splat resolve), see shaders/bdpt_*.comp and
    //    docs/ARCHITECTURE.md.
    // Call Reset() whenever the camera, scene, or settings change;
    // otherwise call Render() once per frame to add one more sample.
    class PathTracer
    {
    public:
        PathTracer();

        void Resize(int width, int height);
        void Reset();
        void Render(const Camera& camera, const BVHBuilder& bvh, const LightTree& lightTree,
                    const LensSystem& lensSystem, const RenderSettings& settings);

        // Schedules the pixel-pupil precomputation pass before the next
        // frame (call after the lens prescription/settings or the
        // resolution changed).
        void MarkPupilsDirty() { m_pupilsDirty = true; }

        const Texture& AccumulationImage() const { return m_accum; }
        uint32_t SampleCount() const { return m_sampleCount; }
        int Width() const { return m_width; }
        int Height() const { return m_height; }

    private:
        Shader m_computeShader;
        Shader m_bdptLightSelShader;
        Shader m_bdptLightShader;
        Shader m_bdptEyeShader;
        Shader m_bdptResolveShader;
        Shader m_lensPupilShader;
        Texture m_accum;
        Buffer m_frameUBO{BufferType::Uniform, 0};

        // BDPT storage: light subpath vertices + per-path counts, and the
        // fixed-point t=1 splat accumulator (see shaders/bdpt_common.glsl).
        Buffer m_lightVertexBuffer{BufferType::ShaderStorage, 8};
        Buffer m_splatBuffer{BufferType::ShaderStorage, 9};
        Buffer m_lightVertCountBuffer{BufferType::ShaderStorage, 11};
        Buffer m_lightSelPdfBuffer{BufferType::ShaderStorage, 12};
        Buffer m_pixelPupilBuffer{BufferType::ShaderStorage, 14};
        uint32_t m_numLightPaths = 0;
        uint32_t m_lightSelPdfCount = 0;
        bool m_pupilsDirty = true;

        // GL_TIME_ELAPSED queries per pass (0=unidir/light, 1=eye,
        // 2=resolve), double-buffered so reading back the previous frame's
        // results never stalls. Enabled via ROYALGL_STATS=1; averages are
        // logged every 128 frames.
        bool m_timersEnabled = false;
        unsigned int m_timerQueries[2][3] = {};
        int m_timerFrame = 0;
        double m_passMsSum[3] = {};
        int m_passMsCount = 0;

        int m_width = 0;
        int m_height = 0;
        uint32_t m_sampleCount = 0;
    };
}
