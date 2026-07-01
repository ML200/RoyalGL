#pragma once

#include <vector>
#include <GL/glew.h>

namespace RoyalGL
{
    // A 2D floating-point texture usable both as a sampler (for display) and
    // as an image2D (for compute shader read/write - i.e. progressive
    // accumulation).
    class Texture
    {
    public:
        Texture() = default;
        Texture(int width, int height, GLenum internalFormat = GL_RGBA32F);
        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;

        // No-op if the size is unchanged. Destroys prior contents otherwise.
        void Resize(int width, int height);

        // Clears all texels to (0,0,0,0); used to reset progressive accumulation.
        void Clear() const;

        void BindImage(GLuint unit, GLenum access = GL_READ_WRITE) const;
        void BindTexture(GLuint unit) const;

        // Reads back the full texture as tightly packed RGBA32F floats
        // (width * height * 4 floats).
        std::vector<float> ReadPixelsFloat() const;

        int Width() const { return m_width; }
        int Height() const { return m_height; }
        GLuint Id() const { return m_id; }
        bool IsValid() const { return m_id != 0; }

    private:
        void Release();
        void Allocate();

        GLuint m_id = 0;
        int m_width = 0;
        int m_height = 0;
        GLenum m_internalFormat = GL_RGBA32F;
    };
}
