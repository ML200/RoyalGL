#pragma once

#include <cstddef>
#include <GL/glew.h>

namespace RoyalGL
{
    enum class BufferType : GLenum
    {
        ShaderStorage = GL_SHADER_STORAGE_BUFFER,
        Uniform = GL_UNIFORM_BUFFER
    };

    // Wraps a GL buffer bound to a fixed indexed binding point matching the
    // `layout(binding = N)` declared in GLSL (an SSBO or a UBO).
    class Buffer
    {
    public:
        Buffer() = default;
        Buffer(BufferType type, GLuint bindingPoint);
        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        // (Re)allocates and uploads. Safe to call every frame; usage should be
        // GL_DYNAMIC_DRAW for data that changes often (e.g. the per-frame UBO)
        // and GL_STATIC_DRAW for data uploaded once per scene load.
        void Upload(const void* data, size_t byteSize, GLenum usage = GL_DYNAMIC_DRAW);

        // Binds this buffer to its configured indexed binding point.
        void BindBase() const;

        // Binds to an explicit binding point instead (used for ping-ponged
        // buffers whose current/previous roles swap every frame).
        void BindBase(GLuint bindingPoint) const;

        // KHR_debug object label: names this buffer in Nsight/RenderDoc
        // captures. The GL id is stable for the object's lifetime, so once
        // is enough.
        void SetLabel(const char* name) const;

        GLuint Id() const { return m_id; }
        size_t SizeBytes() const { return m_sizeBytes; }
        bool IsValid() const { return m_id != 0; }

    private:
        void Release();

        GLuint m_id = 0;
        GLenum m_target = GL_SHADER_STORAGE_BUFFER;
        GLuint m_bindingPoint = 0;
        size_t m_sizeBytes = 0;
    };
}
