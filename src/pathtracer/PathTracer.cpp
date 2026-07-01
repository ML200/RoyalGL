#include "pathtracer/PathTracer.h"
#include "gfx/GPUTypes.h"
#include "gfx/GLCheck.h"

#include <cmath>
#include <filesystem>

#ifndef ROYALGL_SHADER_DIR
#define ROYALGL_SHADER_DIR "shaders/"
#endif

namespace RoyalGL
{
    PathTracer::PathTracer()
        : m_computeShader(Shader::CreateCompute(std::filesystem::path(ROYALGL_SHADER_DIR) / "pathtrace.comp"))
    {
    }

    void PathTracer::Resize(int width, int height)
    {
        if (width == m_width && height == m_height) return;
        m_width = width;
        m_height = height;
        m_accum.Resize(width, height);
        Reset();
    }

    void PathTracer::Reset()
    {
        m_accum.Clear();
        m_sampleCount = 0;
    }

    void PathTracer::Render(const Camera& camera, const BVHBuilder& bvh, const RenderSettings& settings,
                             const CameraSettings& cameraSettings, const LensSystem* lensSystem)
    {
        if (m_width == 0 || m_height == 0) return;
        if (settings.maxSamples > 0 && m_sampleCount >= static_cast<uint32_t>(settings.maxSamples)) return;

        bool lensMode = cameraSettings.mode == CameraMode::LensSystem && lensSystem != nullptr;

        GPUFrameUBO frame{};
        frame.camPos = glm::vec4(camera.position, 0.0f);
        frame.camForward = glm::vec4(camera.Forward(), 0.0f);
        frame.camRight = glm::vec4(camera.Right(), 0.0f);
        frame.camUp = glm::vec4(camera.Up(), 0.0f);

        float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
        float tanHalfFovY = std::tan(glm::radians(camera.verticalFovDegrees) * 0.5f);
        if (!lensMode)
            frame.lensParams = glm::vec4(tanHalfFovY, aspect, 0.0f, 0.0f);
        else
            frame.lensParams = glm::vec4(tanHalfFovY, aspect, static_cast<float>(lensSystem->sensorWidthMm), 1.0f);
        frame.background = glm::vec4(settings.backgroundColor, settings.backgroundIntensity);
        frame.frameInfo = glm::uvec4(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height),
                                      m_sampleCount, static_cast<uint32_t>(settings.maxBounces));
        frame.renderParams = glm::vec4(settings.exposure, 0.0f, 0.0f, 0.0f);

        m_frameUBO.Upload(&frame, sizeof(GPUFrameUBO), GL_DYNAMIC_DRAW);
        m_frameUBO.BindBase();

        bvh.BindAll();
        if (lensMode) lensSystem->BindAll();
        m_accum.BindImage(0, GL_READ_WRITE);

        GLuint groupsX = (static_cast<GLuint>(m_width) + 7u) / 8u;
        GLuint groupsY = (static_cast<GLuint>(m_height) + 7u) / 8u;

        m_computeShader.Use();
        m_computeShader.Dispatch(groupsX, groupsY, 1u);
        GL_CALL(glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));

        m_sampleCount++;
    }
}
