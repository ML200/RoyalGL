#pragma once

#include <GL/glew.h>
#include "gfx/Texture.h"

namespace RoyalGL
{
    // Minimal single-color-attachment FBO wrapper, used to bind an existing
    // Texture (e.g. PathTracer's accumulation image) as a raster target -
    // needed for additive-blended splatting (see pathtracer/LensFlare.h),
    // since compute image-store cannot safely accumulate when multiple
    // invocations write the same pixel in one dispatch. Does not own the
    // attached texture.
    class Framebuffer
    {
    public:
        Framebuffer();
        ~Framebuffer();

        Framebuffer(const Framebuffer&) = delete;
        Framebuffer& operator=(const Framebuffer&) = delete;

        // Attaches `tex` as color attachment 0. Safe to call every frame.
        void AttachColor(const Texture& tex) const;
        void Bind() const;
        static void BindDefault();

    private:
        GLuint m_fbo = 0;
    };
}
