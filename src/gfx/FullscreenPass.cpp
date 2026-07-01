#include "gfx/FullscreenPass.h"
#include "gfx/GLCheck.h"

#include <filesystem>

#ifndef ROYALGL_SHADER_DIR
#define ROYALGL_SHADER_DIR "shaders/"
#endif

namespace RoyalGL
{
    FullscreenPass::FullscreenPass()
        : m_shader(Shader::CreateGraphics(std::filesystem::path(ROYALGL_SHADER_DIR) / "tonemap.vert",
                                           std::filesystem::path(ROYALGL_SHADER_DIR) / "tonemap.frag"))
    {
        GL_CALL(glCreateVertexArrays(1, &m_vao));
    }

    FullscreenPass::~FullscreenPass()
    {
        if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    }

    void FullscreenPass::Draw(const Texture& hdrSource, float exposure, unsigned sampleCount) const
    {
        hdrSource.BindTexture(0);
        m_shader.SetFloat("uExposure", exposure);
        m_shader.SetFloat("uSampleCount", static_cast<float>(sampleCount));
        m_shader.Use();
        glBindVertexArray(m_vao);
        GL_CALL(glDrawArrays(GL_TRIANGLES, 0, 3));
    }
}
