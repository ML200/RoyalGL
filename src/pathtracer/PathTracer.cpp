#include "pathtracer/PathTracer.h"
#include "core/Log.h"
#include "gfx/GPUTypes.h"
#include "gfx/GLCheck.h"

#include <algorithm>
#include <cmath>
#include <chrono>
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

        // sizeof(PixelReservoirs) / sizeof(GBufferPixel) in
        // shaders/restir_common.glsl.
        constexpr size_t kPixelReservoirsBytes = 272;
        constexpr size_t kGBufferPixelBytes = 32;
    }

    PathTracer::PathTracer()
        : m_computeShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "pathtrace.comp")),
          m_bdptLightSelShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_lightsel.comp")),
          m_bdptLightShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_light.comp")),
          m_bdptEyeShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_eye.comp")),
          m_bdptResolveShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "bdpt_resolve.comp")),
          m_lensPupilShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "lens_pupil.comp")),
          m_restirGbufferShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "restir_gbuffer.comp")),
          m_restirCameraShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "restir_camera.comp")),
          m_restirTemporalShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "restir_temporal.comp")),
          m_restirSpatialShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "restir_spatial.comp")),
          m_restirResolveShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "restir_resolve.comp")),
          m_restirDebugShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "restir_debug.comp"))
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
        m_accum.Clear(); // fresh allocation: alpha (per-pixel counts) must start at 0

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
        // No accumulation clear: sample 0 overwrites per pixel (the kernels
        // write prev=0 at sampleIndex 0), so during continuous camera moves
        // the not-yet-retraced rows keep showing the previous image instead
        // of black while tiles progress top to bottom.
        m_sampleCount = 0;
        m_phase = 0;
        m_cursor = 0;
        m_fullFrameNext = true;
        // Drop splats of an abandoned in-flight sample - the next resolve
        // would otherwise add them into the fresh image.
        if (m_splatBuffer.IsValid())
            GL_CALL(glClearNamedBufferData(m_splatBuffer.Id(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));
    }

    void PathTracer::EnsureRestirBuffers()
    {
        if (m_restirWidth == m_width && m_restirHeight == m_height) return;
        m_restirWidth = m_width;
        m_restirHeight = m_height;

        // The reservoir buffer holds 3 regions (two frame-alternating finals
        // + scratch, see restir_common.glsl); the G-buffer two halves.
        size_t pixelCount = size_t(m_width) * size_t(m_height);
        m_reservoirBuffer.Upload(nullptr, 3 * pixelCount * kPixelReservoirsBytes, GL_DYNAMIC_COPY);
        m_gbufferBuffer.Upload(nullptr, 2 * pixelCount * kGBufferPixelBytes, GL_DYNAMIC_COPY);
        // Zero-filled: a zeroed reservoir is a valid empty reservoir
        // (W=0, confidence=0).
        GL_CALL(glClearNamedBufferData(m_reservoirBuffer.Id(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));
        GL_CALL(glClearNamedBufferData(m_gbufferBuffer.Id(), GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));
        m_prevCamValid = false; // stale prev G-buffer after a resize
    }

    void PathTracer::Render(const Camera& camera, const BVHBuilder& bvh, const LightTree& lightTree,
                             const LensSystem& lensSystem, const RenderSettings& settings)
    {
        if (m_width == 0 || m_height == 0) return;
        if (settings.maxSamples > 0 && m_sampleCount >= static_cast<uint32_t>(settings.maxSamples)) return;

        bool lensMode = settings.cameraMode == CameraMode::Lens;
        // ReSTIR needs a deterministic pinhole primary hit; lens mode falls
        // back to plain progressive BDPT. ReSTIR always uses the
        // bidirectional pipeline and full-frame dispatch.
        bool restirActive = settings.enableRestir && !lensMode;
        bool bidir = settings.enableBidir || restirActive;
        if (restirActive)
        {
            EnsureRestirBuffers(); // may invalidate m_prevCamValid on resize
            m_fullFrameNext = true;
        }

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
        frame.renderParams = glm::vec4(settings.exposure, lightTree.TotalPower(), 0.0f,
                                        settings.lens.flareIntensity);
        frame.lightInfo = glm::uvec4(lightTree.LightCount(), settings.enableNEE ? 1u : 0u, m_numLightPaths,
                                     static_cast<uint32_t>(std::max(settings.lens.flareSamples, 1)));

        float aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
        float sensorHalfH = settings.lens.sensorHeightMm * 0.5f;
        frame.lensParams = glm::vec4(sensorHalfH * aspectRatio, sensorHalfH,
                                     lensSystem.FrontVertexZMm(), lensSystem.RearVertexZMm());
        frame.lensParams2 = glm::vec4(lensMode ? 1.0f : 0.0f, settings.lens.enableFlare ? 1.0f : 0.0f,
                                      lensSystem.RearSemiDiameterMm(), lensSystem.FrontSemiDiameterMm());
        frame.lensParams3 = glm::vec4(settings.lens.enableDiffraction ? 1.0f : 0.0f,
                                      settings.lens.diffractionIntensity,
                                      settings.lens.diffractionEdgeWidthMm, 0.0f);

        // Previous frame's camera for ReSTIR reprojection; identical to the
        // current camera until a ReSTIR frame has completed.
        frame.prevCamPos = m_prevCamValid ? m_prevCamPos : frame.camPos;
        frame.prevCamForward = m_prevCamValid ? m_prevCamForward : frame.camForward;
        frame.prevCamRight = m_prevCamValid ? m_prevCamRight : frame.camRight;
        frame.prevCamUp = m_prevCamValid ? m_prevCamUp : frame.camUp;
        frame.prevCameraParams = m_prevCamValid ? m_prevCameraParams : frame.cameraParams;
        // z/w of prevCameraParams are unused by the projection helpers and
        // carry the spatial reuse parameters instead.
        frame.prevCameraParams.z = settings.restirSpatialRadius;
        frame.prevCameraParams.w = static_cast<float>(settings.restirSpatialNeighbors);
        uint32_t restirFlags = (restirActive ? 1u : 0u)
                             | (settings.restirTemporal ? 2u : 0u)
                             | (settings.restirSpatial ? 4u : 0u)
                             | (settings.accumulate ? 8u : 0u);
        frame.restirParams = glm::uvec4(static_cast<uint32_t>(settings.restirDebugView),
                                        restirFlags, m_frameCounter, m_restirParity);

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
        bool allowTimer = m_timersEnabled && !m_fullFrameNext; // one query per frame max
        auto beginTimer = [&](int slot) { if (allowTimer) glBeginQuery(GL_TIME_ELAPSED, m_timerQueries[q][slot]); };
        auto endTimer = [&]() { if (allowTimer) glEndQuery(GL_TIME_ELAPSED); };

        if (lensMode && m_pupilsDirty)
        {
            // Pixel-pupil precomputation (Steinert et al. 2011 sec. 4.2),
            // once per lens/sensor change.
            m_lensPupilShader.Use();
            m_lensPupilShader.Dispatch(groupsX, groupsY, 1u);
            GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            m_pupilsDirty = false;
        }

        if (restirActive)
        {
            // ------------------------- ReSTIR frame path (per-frame) -----
            // G-buffer -> initial candidates (Alg. 2, s<=1) -> temporal ->
            // spatial -> resolve. Regions/halves are selected inside the
            // shaders via restirParams - see restir_common.glsl.
            m_reservoirBuffer.BindBase();
            m_gbufferBuffer.BindBase();
            m_lightVertexBuffer.BindBase();
            m_splatBuffer.BindBase();
            m_lightVertCountBuffer.BindBase();
            if (m_lightSelPdfCount != std::max(lightTree.LightCount(), 1u))
            {
                m_lightSelPdfCount = std::max(lightTree.LightCount(), 1u);
                m_lightSelPdfBuffer.Upload(nullptr, size_t(m_lightSelPdfCount) * sizeof(float), GL_DYNAMIC_COPY);
            }
            m_lightSelPdfBuffer.BindBase();

            // Camera-anchored light-selection pdf cache (receiver-
            // independent NEE pdfs, required for replayable shifts).
            if (lightTree.LightCount() > 0)
            {
                m_bdptLightSelShader.Use();
                m_bdptLightSelShader.Dispatch((lightTree.LightCount() + 63u) / 64u, 1u, 1u);
                GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            }

            // Deterministic V-buffer for this frame's camera.
            m_restirGbufferShader.Use();
            m_restirGbufferShader.Dispatch(groupsX, groupsY, 1u);
            GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));

            m_restirCameraShader.Use();
            m_restirCameraShader.Dispatch(groupsX, groupsY, 1u);
            GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));

            if (settings.restirTemporal)
            {
                m_restirTemporalShader.Use();
                m_restirTemporalShader.Dispatch(groupsX, groupsY, 1u);
                GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            }
            if (settings.restirSpatial)
            {
                m_restirSpatialShader.Use();
                m_restirSpatialShader.Dispatch(groupsX, groupsY, 1u);
                GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            }

            m_restirResolveShader.Use();
            m_restirResolveShader.Dispatch(groupsX, groupsY, 1u);
            m_sampleCount++;
        }

        // ---------------------------------------------- tiled dispatch ----
        // One slice of the current sample per UI frame, sized so the frame
        // stays near kTargetFrameMs - the swap no longer waits on a full
        // sample and ImGui stays responsive at any render cost. Slice sizes
        // adapt to the measured frame time.
        constexpr double kTargetFrameMs = 30.0;
        double nowMs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count()) * 1e-3;
        if (m_lastRenderTime > 0.0)
        {
            double frameMs = nowMs - m_lastRenderTime;
            double f = std::clamp(kTargetFrameMs / std::max(frameMs, 1.0), 0.6, 1.4);
            m_rowsPerTile = std::clamp(static_cast<uint32_t>(m_rowsPerTile * f + 0.5), 16u,
                                        static_cast<uint32_t>(m_height));
            m_pathsPerChunk = std::clamp(static_cast<uint32_t>(m_pathsPerChunk * f + 0.5), 4096u, m_numLightPaths);
        }
        m_lastRenderTime = nowMs;

        auto uploadFrame = [&](uint32_t offset, uint32_t end)
        {
            frame.cameraParams.w = static_cast<float>(offset);
            frame.renderParams.z = static_cast<float>(end);
            m_frameUBO.Upload(&frame, sizeof(GPUFrameUBO), GL_DYNAMIC_DRAW);
            m_frameUBO.BindBase();
        };

        uint32_t sampleAtEntry = m_sampleCount;
        if (!restirActive)
        {
        do
        {
        if (!bidir)
        {
            uint32_t rows = std::min(m_rowsPerTile, static_cast<uint32_t>(m_height) - m_cursor);
            uploadFrame(m_cursor, m_cursor + rows);
            beginTimer(0);
            m_computeShader.Use();
            m_computeShader.Dispatch(groupsX, (rows + 7u) / 8u, 1u);
            endTimer();
            m_cursor += rows;
            if (m_cursor >= static_cast<uint32_t>(m_height))
            {
                m_cursor = 0;
                m_sampleCount++;
            }
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

            if (m_phase == 0)
            {
                // Sample start: refresh the camera-anchored selection pdf
                // cache, then trace a chunk of light subpaths.
                if (m_cursor == 0 && lightTree.LightCount() > 0)
                {
                    uploadFrame(0, 0);
                    m_bdptLightSelShader.Use();
                    m_bdptLightSelShader.Dispatch((lightTree.LightCount() + 63u) / 64u, 1u, 1u);
                    GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
                }

                uint32_t chunk = std::min(m_pathsPerChunk, m_numLightPaths - m_cursor);
                uploadFrame(m_cursor, m_cursor + chunk);
                beginTimer(0);
                m_bdptLightShader.Use();
                m_bdptLightShader.Dispatch((chunk + 63u) / 64u, 1u, 1u);
                endTimer();
                GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
                m_cursor += chunk;
                if (m_cursor >= m_numLightPaths)
                {
                    m_phase = 1;
                    m_cursor = 0;
                }
            }
            else
            {
                uint32_t rows = std::min(m_rowsPerTile, static_cast<uint32_t>(m_height) - m_cursor);
                uploadFrame(m_cursor, m_cursor + rows);
                beginTimer(0);
                m_bdptEyeShader.Use();
                m_bdptEyeShader.Dispatch(groupsX, (rows + 7u) / 8u, 1u);
                endTimer();
                m_cursor += rows;
                if (m_cursor >= static_cast<uint32_t>(m_height))
                {
                    // Sample complete: drain the splat buffer into the image.
                    GL_CALL(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT));
                    uploadFrame(0, static_cast<uint32_t>(m_height));
                    m_bdptResolveShader.Use();
                    m_bdptResolveShader.Dispatch(groupsX, groupsY, 1u);
                    m_phase = 0;
                    m_cursor = 0;
                    m_sampleCount++;
                }
            }
        }
        } while (m_fullFrameNext && m_sampleCount == sampleAtEntry);
        m_fullFrameNext = false;
        }

        if (restirActive)
        {
            if (settings.restirDebugView != 0)
            {
                // Overwrite the accumulator with the selected visualization
                // (write-after-write on the image, hence the barrier).
                GL_CALL(glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT));
                m_restirDebugShader.Use();
                m_restirDebugShader.Dispatch(groupsX, groupsY, 1u);
            }

            m_prevCamPos = frame.camPos;
            m_prevCamForward = frame.camForward;
            m_prevCamRight = frame.camRight;
            m_prevCamUp = frame.camUp;
            m_prevCameraParams = frame.cameraParams;
            m_prevCamValid = true;
            m_restirParity = 1u - m_restirParity;
        }
        m_frameCounter++;

        GL_CALL(glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));

        if (m_timersEnabled)
        {
            // Read the previous frame's queries (results are ready by now).
            if (m_timerFrame > 0)
            {
                int prev = 1 - q;
                GLuint64 ns = 0;
                glGetQueryObjectui64v(m_timerQueries[prev][0], GL_QUERY_RESULT, &ns);
                m_passMsSum[0] += static_cast<double>(ns) * 1e-6;
                m_passMsCount++;
                if (m_passMsCount == 128)
                {
                    ROYALGL_LOG_INFO("GPU tile time (avg over 128 frames): ", m_passMsSum[0] / 128.0, "ms");
                    m_passMsSum[0] = m_passMsSum[1] = m_passMsSum[2] = 0.0;
                    m_passMsCount = 0;
                }
            }
            m_timerFrame++;
        }
    }
}
