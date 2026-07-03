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

        // Zeroes the ReSTIR reservoir regions (temporal history included).
        // Called when an instance-edit rebuild burst settles so the stale-
        // history transient can't bake into the restarted accumulation.
        void ClearRestirHistory();
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
        Shader m_restirGbufferShader;
        Shader m_restirLightShader;
        Shader m_restirCameraShader;
        Shader m_restirTemporalShader;
        Shader m_restirCausticShiftShader;
        Shader m_restirCausticMergeShader;
        Shader m_restirSpatialShader;
        Shader m_restirResolveShader;
        Shader m_restirDebugShader;
        // Wavefront ReSTIR kernels (docs/RESTIR_BDPT_PLAN.md Phase 7): the
        // camera RIS / temporal / spatial megakernels split at every ray
        // boundary, with GPU-driven compacted queues. The megakernels above
        // remain as the A/B + low-binding-count fallback
        // (ROYALGL_RESTIR_WAVEFRONT=0).
        Shader m_wfCamInitShader;
        Shader m_wfCamShadeShader;
        Shader m_wfCamTraceShader;
        Shader m_wfCamShadowShader;
        Shader m_wfCamFinalShader;
        Shader m_wfShiftStepShader;
        Shader m_wfShiftTraceShader;
        Shader m_wfShiftShadowShader;
        Shader m_wfTInitShader;
        Shader m_wfTMergeShader;
        Shader m_wfSInitShader;
        Shader m_wfSMergeShader;
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

        // ReSTIR state (docs/RESTIR_BDPT_PLAN.md). SSBO bindings above 15
        // don't exist on common hardware, so each per-pixel buffer holds
        // BOTH ping-pong halves (2 x pixelCount entries); the halves swap
        // current/previous roles every frame via the parity uploaded in
        // restirParams.w. Allocated lazily on the first ReSTIR frame -
        // together ~600 B/pixel.
        Buffer m_reservoirBuffer{BufferType::ShaderStorage, 15};
        Buffer m_gbufferBuffer{BufferType::ShaderStorage, 0};
        // Light Reservoir Map (Phase 2, shaders/restir_lrm.glsl): staged t=1
        // candidate entries + per-pixel linked-list heads (slot 0 is the
        // entry allocator). Reclaims the lens-only bindings 13/14 - ReSTIR
        // is pinhole-only, so the lens shaders never run in the same frame.
        Buffer m_lrmEntryBuffer{BufferType::ShaderStorage, 13};
        Buffer m_lrmHeadBuffer{BufferType::ShaderStorage, 14};
        // Wavefront queues (shaders/restir_wf_common.glsl): per-pixel/job
        // scratch arena, plus the compacted work-item queues whose first
        // 512 bytes are 32 dispatch-control entries that double as
        // glDispatchComputeIndirect args (the queue buffer is also bound as
        // GL_DISPATCH_INDIRECT_BUFFER). Bindings 16-17 exceed the
        // 16-binding portability budget - wavefront mode is gated on
        // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS >= 18 and
        // ROYALGL_RESTIR_WAVEFRONT (default on).
        Buffer m_wfArenaBuffer{BufferType::ShaderStorage, 16};
        Buffer m_wfQueueBuffer{BufferType::ShaderStorage, 17};
        // Per-pass wavefront selection (ROYALGL_RESTIR_WAVEFRONT): 0 = all
        // megakernel, 1/unset = all wavefront, else a bitmask - bit0
        // camera RIS, bit1 temporal, bit2 spatial (e.g. 6 = wavefront
        // shifts + megakernel camera, the fastest measured combination on
        // the RTX 5090 at 1600x900; see RESTIR_BDPT_PLAN.md Phase 7).
        uint32_t m_wavefrontMask = 7;
        bool m_wavefrontRestir = true; // any bit set (buffers + gate)
        // ROYALGL_RESTIR_CAUSTIC=0: skip the caustic temporal passes
        // (shift + merge) - the caustic reservoir then holds only the
        // per-frame canonical RIS result. Isolation switch for chasing
        // temporal transients to the caustic vs the path reservoir.
        bool m_causticPassesEnabled = true;
        uint32_t m_restirParity = 0;
        int m_restirWidth = 0;  // resolution the ReSTIR buffers were sized for
        int m_restirHeight = 0;
        bool m_prevCamValid = false;
        glm::vec4 m_prevCamPos{0.0f};
        glm::vec4 m_prevCamForward{0.0f};
        glm::vec4 m_prevCamRight{0.0f};
        glm::vec4 m_prevCamUp{0.0f};
        glm::vec4 m_prevCameraParams{0.0f};
        uint32_t m_frameCounter = 0; // never reset - decorrelates ReSTIR frames

        void EnsureRestirBuffers();

        // GL_TIME_ELAPSED queries per pass, double-buffered so reading back
        // the previous frame's results never stalls. Enabled via
        // ROYALGL_STATS=1; averages are logged every 128 frames. Slot 0 is
        // the tiled classic path's single timer; ReSTIR mode times its full
        // pass graph (slots = kRestirPassNames order, mask tracks which
        // slots ran that frame).
        static constexpr int kTimerSlots = 8;
        bool m_timersEnabled = false;
        unsigned int m_timerQueries[2][kTimerSlots] = {};
        unsigned int m_timerMask[2] = {};
        int m_timerFrame = 0;
        double m_passMsSum[kTimerSlots] = {};
        int m_passMsCount = 0;

        // Tiled dispatch: each Render() call submits only a slice of the
        // current sample (a light-path chunk or a run of eye rows), sized
        // adaptively so the UI keeps its own framerate while a full sample
        // spans several frames. Per-pixel sample counts live in the
        // accumulator's alpha channel.
        int m_phase = 0;        // 0 = light subpaths (bidir), 1 = eye rows
        uint32_t m_cursor = 0;  // paths done (phase 0) / rows done (phase 1)
        uint32_t m_rowsPerTile = 64;
        uint32_t m_pathsPerChunk = 32768;
        double m_lastRenderTime = 0.0;
        // Set by Reset(): the next frame runs its whole sample in one go so
        // the image updates every frame while the camera moves (a per-frame
        // reset would otherwise only ever retrace the topmost tile).
        bool m_fullFrameNext = true;

        int m_width = 0;
        int m_height = 0;
        uint32_t m_sampleCount = 0;
    };
}
