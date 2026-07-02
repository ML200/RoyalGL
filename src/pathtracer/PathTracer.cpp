#include "pathtracer/PathTracer.h"
#include "core/Log.h"
#include "gfx/GPUTypes.h"
#include "gfx/GLCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#ifndef ROYALGL_SHADER_DIR
#define ROYALGL_SHADER_DIR "shaders/"
#endif

namespace RoyalGL
{
    namespace
    {
        // Must match BDPT_MAX_LIGHT_VERTS in shaders/bdpt_common.glsl.
        constexpr uint32_t kMaxLightVerts = 8;
        // sizeof(LightVertex) in shaders/bdpt_common.glsl: 4 x vec4.
        constexpr size_t kLightVertexBytes = 64;
        // Cap on concurrent light subpaths: enough that every pixel gets a
        // statistically fresh subpath (re-drawn by hash every frame), small
        // enough that the vertex buffer stays ~130 MB at worst.
        constexpr uint32_t kMaxLightPaths = 262144;
    }

    PathTracer::PathTracer()
        : m_computeShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "pathtrace.comp")),
          m_bdptLightSelShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_lightsel.comp")),
          m_bdptLightShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_light.comp")),
          m_bdptEyeShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_eye.comp")),
          m_bdptResolveShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_resolve.comp")),
          m_lensPupilShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "lens_pupil.comp"))
    {
        m_timersEnabled = (std::getenv("ROYALGL_STATS") != nullptr);
        if (m_timersEnabled)
            GL_CALL(glGenQueries(6, &m_timerQueries[0][0]));
    }

    void PathTracer::Resize(int width, int height)
    {
        if (width == m_width && height == m_height) return;
        m_width = width;
        m_height = height;
        m_accum.Resize(width, height);

        uint32_t pixelCount = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
        m_numLightPaths = std::min(pixelCount, kMaxLightPaths);
        m_lightVertexBuffer.Upload(nullptr, size_t(m_numLightPaths) * kMaxLightVerts * kLightVertexBytes,
                                   GL_DYNAMIC_COPY);
        m_lightVertCountBuffer.Upload(nullptr, size_t(m_numLightPaths) * sizeof(uint32_t), GL_DYNAMIC_COPY);
        m_splatBuffer.Upload(nullptr, size_t(pixelCount) * 3 * sizeof(uint32_t), GL_DYNAMIC_COPY);
        m_pixelPupilBuffer.Upload(nullptr, size_t(pixelCount) * 4 * sizeof(float), GL_DYNAMIC_COPY);
        m_pupilsDirty = true;
        // The splat buffer must start zeroed: the resolve pass drains and
        // re-zeroes it each frame, but nothing has cleared the fresh
        // allocation yet.
        GL_CALL(glClearNamedBufferData(m_splatBuffer.Id(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));

        Reset();
    }

    void PathTracer::Reset()
    {
        m_accum.Clear();
        m_sampleCount = 0;
    }

    void PathTracer::Render(const Camera& camera, const BVHBuilder& bvh, const LightTree& lightTree,
                             const LensSystem& lensSystem, const RenderSettings& settings)
    {
        if (m_width == 0 || m_height == 0) return;
        if (settings.maxSamples > 0 && m_sampleCount >= static_cast<uint32_t>(settings.maxSamples)) return;

        bool lensMode = settings.cameraMode == CameraMode::Lens;
        bool bidir = settings.enableBidir;

        GPUFrameUBO frame{};
        frame.camPos = glm::vec4(camera.position, 0.0f);
        frame.camForward = glm::vec4(camera.Forward(), 0.0f);
        frame.camRight = glm::vec4(camera.Right(), 0.0f);
        frame.camUp = glm::vec4(camera.Up(), 0.0f);

        float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
        float tanHalfFovY = std::tan(glm::radians(camera.verticalFovDegrees) * 0.5f);
        frame.cameraParams = glm::vec4(tanHalfFovY, aspect, lensSystem.EffectiveFocalLengthMm(), 0.0f);
        frame.background = glm::vec4(settings.backgroundColor, settings.backgroundIntensity);
        frame.frameInfo = glm::uvec4(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height),
                                      m_sampleCount, static_cast<uint32_t>(settings.maxBounces));
        frame.renderParams = glm::vec4(settings.exposure, lightTree.TotalPower(), 0.0f, 0.0f);
        frame.lightInfo = glm::uvec4(lightTree.LightCount(), settings.enableNEE ? 1u : 0u, m_numLightPaths, 0u);

        float aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
        float sensorHalfH = settings.lens.sensorHeightMm * 0.5f;
        frame.lensParams = glm::vec4(sensorHalfH * aspectRatio, sensorHalfH,
                                     lensSystem.FrontVertexZMm(), lensSystem.RearVertexZMm());
        frame.lensParams2 = glm::vec4(lensMode ? 1.0f : 0.0f, settings.lens.enableFlare ? 1.0f : 0.0f,
                                      lensSystem.RearSemiDiameterMm(), lensSystem.FrontSemiDiameterMm());

        m_frameUBO.Upload(&frame, sizeof(GPUFrameUBO), GL_DYNAMIC_DRAW);
        m_frameUBO.BindBase();

        bvh.BindAll();
        lightTree.BindAll();
        lensSystem.Bind();
        m_pixelPupilBuffer.BindBase();
        m_accum.BindImage(0, GL_READ_WRITE);

        GLuint groupsX = (static_cast<GLuint>(m_width) + 7u) / 8u;
        GLuint groupsY = (static_cast<GLuint>(m_height) + 7u) / 8u;

        int q = m_timerFrame & 1;
        auto beginTimer = [&](int slot) { if (m_timersEnabled) glBeginQuery(GL_TIME_ELAPSED, m_timerQueries[q][slot]); };
        auto endTimer = [&]() { if (m_timersEnabled) glEndQuery(GL_TIME_ELAPSED); };

        if (lensMode && m_pupilsDirty)
        {
            // Pixel-pupil precomputation (Steinert et al. 2011 sec. 4.2),
            // once per lens/sensor change.
            m_lensPupilShader.Use();
            m_lensPupilShader.Dispatch(groupsX, groupsY, 1u);
            GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            m_pupilsDirty = false;
        }

        if (!bidir)
        {
            beginTimer(0);
            m_computeShader.Use();
            m_computeShader.Dispatch(groupsX, groupsY, 1u);
            endTimer();
        }
        else
        {
            m_lightVertexBuffer.BindBase();
            m_splatBuffer.BindBase();
            m_lightVertCountBuffer.BindBase();

            if (m_lightSelPdfCount != std::max(lightTree.LightCount(), 1u))
            {
                m_lightSelPdfCount = std::max(lightTree.LightCount(), 1u);
                m_lightSelPdfBuffer.Upload(nullptr, size_t(m_lightSelPdfCount) * sizeof(float), GL_DYNAMIC_COPY);
            }
            m_lightSelPdfBuffer.BindBase();

            // Pass 0: cache the camera-anchored per-light selection pdf
            // (pixel-independent, so once per frame).
            if (lightTree.LightCount() > 0)
            {
                m_bdptLightSelShader.Use();
                m_bdptLightSelShader.Dispatch((lightTree.LightCount() + 63u) / 64u, 1u, 1u);
                GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            }

            // Pass 1: light subpaths (vertex storage + t=1 splats).
            beginTimer(0);
            m_bdptLightShader.Use();
            m_bdptLightShader.Dispatch((m_numLightPaths + 63u) / 64u, 1u, 1u);
            endTimer();
            // Light vertices/counts written above are SSBO-read by the eye
            // pass; splats only matter after the eye pass, same barrier.
            GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));

            // Pass 2: eye subpaths, connections, image accumulation.
            beginTimer(1);
            m_bdptEyeShader.Use();
            m_bdptEyeShader.Dispatch(groupsX, groupsY, 1u);
            endTimer();
            // Resolve reads the splat SSBO and read-modify-writes the same
            // accumulation image the eye pass just image-stored.
            GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT));

            // Pass 3: drain splats into the accumulation image.
            beginTimer(2);
            m_bdptResolveShader.Use();
            m_bdptResolveShader.Dispatch(groupsX, groupsY, 1u);
            endTimer();
        }
        GL_CALL(glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));

        if (m_timersEnabled)
        {
            // Read the previous frame's queries (results are ready by now).
            if (m_timerFrame > 0)
            {
                int prev = 1 - q;
                int slots = bidir ? 3 : 1;
                for (int s = 0; s < slots; ++s)
                {
                    GLuint64 ns = 0;
                    glGetQueryObjectui64v(m_timerQueries[prev][s], GL_QUERY_RESULT, &ns);
                    m_passMsSum[s] += static_cast<double>(ns) * 1e-6;
                }
                m_passMsCount++;
                if (m_passMsCount == 128)
                {
                    if (bidir)
                        ROYALGL_LOG_INFO("GPU pass times (avg over 128): light=", m_passMsSum[0] / 128.0,
                                         "ms eye=", m_passMsSum[1] / 128.0, "ms resolve=", m_passMsSum[2] / 128.0, "ms");
                    else
                        ROYALGL_LOG_INFO("GPU pass times (avg over 128): unidir=", m_passMsSum[0] / 128.0, "ms");
                    m_passMsSum[0] = m_passMsSum[1] = m_passMsSum[2] = 0.0;
                    m_passMsCount = 0;
                }
            }
            m_timerFrame++;
        }

        m_sampleCount++;
    }
}
