#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include "gfx/Shader.h"
#include "gfx/Buffer.h"
#include "gfx/Framebuffer.h"
#include "gfx/Texture.h"
#include "pathtracer/LightList.h"
#include "pathtracer/RenderSettings.h"

namespace RoyalGL
{
    // Owns the forward light-tracing (flare/ghost + aperture diffraction)
    // compute pass and the additive-blend splat draw pass that accumulates
    // its output into PathTracer's accumulation texture. See
    // docs/ARCHITECTURE.md for the full per-frame sequence and the GL
    // barriers required around this pass.
    class LensFlare
    {
    public:
        LensFlare();
        ~LensFlare();

        LensFlare(const LensFlare&) = delete;
        LensFlare& operator=(const LensFlare&) = delete;

        // Clears this frame's splat counter. Call before TraceLightPaths().
        void ResetSplatBuffer();

        // Dispatches the light-tracing compute kernel. Assumes
        // PathTracer::Render() has already bound FrameUBO (binding 0) and,
        // in lens mode, the LensSystem SSBO (binding 5) this frame.
        void TraceLightPaths(const LightList& lights, const RenderSettings& settings);

        // Binds `accum` as an FBO color attachment and additively blends
        // every splat record into it. Restores the default framebuffer
        // binding before returning is the caller's responsibility.
        void SplatToAccumulation(const Texture& accum, glm::ivec2 imageSize);

        static constexpr uint32_t kMaxSplats = 65536;

    private:
        Shader m_traceShader;
        Shader m_splatShader;
        GLuint m_splatVao = 0;

        Buffer m_splatBuffer{BufferType::ShaderStorage, 8};
        Buffer m_splatCounterBuffer{BufferType::ShaderStorage, 9};
        Framebuffer m_framebuffer;
    };
}
