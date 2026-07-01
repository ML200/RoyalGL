#include "pathtracer/LensFlare.h"
#include "gfx/GPUTypes.h"
#include "gfx/GLCheck.h"

#include <filesystem>
#include <vector>

#ifndef ROYALGL_SHADER_DIR
#define ROYALGL_SHADER_DIR "shaders/"
#endif

namespace RoyalGL
{
    LensFlare::LensFlare()
        : m_traceShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "lens_flare.comp")),
          m_splatShader(Shader::CreateGraphics(std::filesystem::path(ROYALGL_SHADER_DIR) / "lens_flare_splat.vert",
                                                std::filesystem::path(ROYALGL_SHADER_DIR) / "lens_flare_splat.frag"))
    {
        GL_CALL(glCreateVertexArrays(1, &m_splatVao));

        // Preallocate the splat buffer at fixed capacity once; ResetSplatBuffer()
        // just clears the counter every frame rather than reallocating.
        std::vector<GPUSplat> zeroSplats(kMaxSplats);
        m_splatBuffer.Upload(zeroSplats.data(), zeroSplats.size() * sizeof(GPUSplat), GL_DYNAMIC_DRAW);
        uint32_t zeroCounter = 0;
        m_splatCounterBuffer.Upload(&zeroCounter, sizeof(zeroCounter), GL_DYNAMIC_DRAW);
    }

    LensFlare::~LensFlare()
    {
        if (m_splatVao != 0) glDeleteVertexArrays(1, &m_splatVao);
    }

    void LensFlare::ResetSplatBuffer()
    {
        uint32_t zero = 0;
        m_splatCounterBuffer.Upload(&zero, sizeof(zero), GL_DYNAMIC_DRAW);

        // Clearing only the counter isn't enough: slots at or beyond this
        // frame's (lower) count still hold a PREVIOUS frame's valid record
        // (the compute kernel only ever writes indices [0, count) via
        // atomicAdd, it never invalidates leftover slots from a frame that
        // had a higher count) - without this, those stale splats get
        // redrawn every subsequent frame, silently over-accumulating. A
        // full-buffer GPU-side zero-clear is cheap (one driver call) and
        // avoids the CPU readback/stall a size-aware partial clear would need.
        GL_CALL(glClearNamedBufferData(m_splatBuffer.Id(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));
    }

    void LensFlare::TraceLightPaths(const LightList& lights, const RenderSettings& settings)
    {
        if (lights.LightCount() == 0) return;

        lights.BindAll();
        m_splatBuffer.BindBase();
        m_splatCounterBuffer.BindBase();

        m_traceShader.SetUint("uSamplesPerDispatch", static_cast<unsigned int>(settings.flareSamplesPerFrame));
        m_traceShader.SetUint("uMaxSplats", kMaxSplats);
        m_traceShader.SetFloat("uFlareIntensity", settings.flareIntensity);
        m_traceShader.SetInt("uEnableDiffraction", settings.enableDiffraction ? 1 : 0);
        m_traceShader.SetFloat("uDiffractionEdgeEpsilonMM", settings.diffractionEdgeEpsilonMM);
        m_traceShader.SetFloat("uDiffractionBranchProbability", settings.diffractionBranchProbability);
        m_traceShader.SetFloat("uDiffractionIntensity", settings.diffractionIntensity);

        m_traceShader.Use();
        uint32_t groups = (static_cast<uint32_t>(settings.flareSamplesPerFrame) + 63u) / 64u;
        m_traceShader.Dispatch(groups, 1u, 1u);
    }

    void LensFlare::SplatToAccumulation(const Texture& accum, glm::ivec2 imageSize)
    {
        m_framebuffer.AttachColor(accum);
        m_framebuffer.Bind();

        GL_CALL(glViewport(0, 0, imageSize.x, imageSize.y));
        GL_CALL(glEnable(GL_PROGRAM_POINT_SIZE));
        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE));
        GL_CALL(glBlendEquation(GL_FUNC_ADD));
        GL_CALL(glDisable(GL_DEPTH_TEST));

        m_splatShader.SetIVec2("uImageSize", imageSize);
        m_splatShader.Use();
        GL_CALL(glBindVertexArray(m_splatVao));
        GL_CALL(glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(kMaxSplats)));

        GL_CALL(glDisable(GL_BLEND));
        GL_CALL(glDisable(GL_PROGRAM_POINT_SIZE));
    }
}
