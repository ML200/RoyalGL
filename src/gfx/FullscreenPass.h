#pragma once

#include <GL/glew.h>
#include "gfx/Shader.h"
#include "gfx/Texture.h"

namespace RoyalGL
{
    // Renders a full-screen triangle that samples an HDR texture and applies
    // exposure + ACES tonemapping + gamma correction to the currently bound
    // framebuffer (the caller is responsible for binding the default
    // framebuffer and setting the viewport beforehand).
    class FullscreenPass
    {
    public:
        FullscreenPass();
        ~FullscreenPass();

        FullscreenPass(const FullscreenPass&) = delete;
        FullscreenPass& operator=(const FullscreenPass&) = delete;

        // `sampleCount` is the number of accumulated samples (used to average
        // the accumulation buffer before tonemapping); pass at least 1.
        void Draw(const Texture& hdrSource, float exposure, unsigned sampleCount) const;

    private:
        Shader m_shader;
        GLuint m_vao = 0;
    };
}
