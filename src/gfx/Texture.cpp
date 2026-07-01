#include "gfx/Texture.h"
#include "gfx/GLCheck.h"

namespace RoyalGL
{
    void Texture::Allocate()
    {
        GL_CALL(glCreateTextures(GL_TEXTURE_2D, 1, &m_id));
        glTextureParameteri(m_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // 1 mip level, immutable storage - required for image load/store and
        // for glBindImageTexture to work reliably.
        GL_CALL(glTextureStorage2D(m_id, 1, m_internalFormat, m_width, m_height));
    }

    void Texture::Release()
    {
        if (m_id != 0)
        {
            glDeleteTextures(1, &m_id);
            m_id = 0;
        }
    }

    Texture::Texture(int width, int height, GLenum internalFormat)
        : m_width(width), m_height(height), m_internalFormat(internalFormat)
    {
        Allocate();
    }

    Texture::~Texture()
    {
        Release();
    }

    Texture::Texture(Texture&& other) noexcept
        : m_id(other.m_id), m_width(other.m_width), m_height(other.m_height), m_internalFormat(other.m_internalFormat)
    {
        other.m_id = 0;
    }

    Texture& Texture::operator=(Texture&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_id = other.m_id;
            m_width = other.m_width;
            m_height = other.m_height;
            m_internalFormat = other.m_internalFormat;
            other.m_id = 0;
        }
        return *this;
    }

    void Texture::Resize(int width, int height)
    {
        if (width == m_width && height == m_height)
            return;

        // Immutable storage textures cannot be resized in place - full
        // recreate is correct here, this is not a hot path.
        Release();
        m_width = width;
        m_height = height;
        Allocate();
    }

    void Texture::Clear() const
    {
        GL_CALL(glClearTexImage(m_id, 0, GL_RGBA, GL_FLOAT, nullptr));
    }

    void Texture::BindImage(GLuint unit, GLenum access) const
    {
        GL_CALL(glBindImageTexture(unit, m_id, 0, GL_FALSE, 0, access, m_internalFormat));
    }

    void Texture::BindTexture(GLuint unit) const
    {
        GL_CALL(glBindTextureUnit(unit, m_id));
    }

    std::vector<float> Texture::ReadPixelsFloat() const
    {
        std::vector<float> data(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4);
        GLsizei bufferSizeBytes = static_cast<GLsizei>(data.size() * sizeof(float));
        GL_CALL(glGetTextureImage(m_id, 0, GL_RGBA, GL_FLOAT, bufferSizeBytes, data.data()));
        return data;
    }
}
