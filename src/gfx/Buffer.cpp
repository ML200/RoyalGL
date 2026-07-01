#include "gfx/Buffer.h"
#include "gfx/GLCheck.h"

namespace RoyalGL
{
    Buffer::Buffer(BufferType type, GLuint bindingPoint)
        : m_target(static_cast<GLenum>(type)), m_bindingPoint(bindingPoint)
    {
        GL_CALL(glCreateBuffers(1, &m_id));
    }

    Buffer::~Buffer()
    {
        if (m_id != 0) glDeleteBuffers(1, &m_id);
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : m_id(other.m_id), m_target(other.m_target), m_bindingPoint(other.m_bindingPoint), m_sizeBytes(other.m_sizeBytes)
    {
        other.m_id = 0;
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_id = other.m_id;
            m_target = other.m_target;
            m_bindingPoint = other.m_bindingPoint;
            m_sizeBytes = other.m_sizeBytes;
            other.m_id = 0;
        }
        return *this;
    }

    void Buffer::Release()
    {
        if (m_id != 0) glDeleteBuffers(1, &m_id);
    }

    void Buffer::Upload(const void* data, size_t byteSize, GLenum usage)
    {
        GL_CALL(glNamedBufferData(m_id, static_cast<GLsizeiptr>(byteSize), data, usage));
        m_sizeBytes = byteSize;
    }

    void Buffer::BindBase() const
    {
        GL_CALL(glBindBufferBase(m_target, m_bindingPoint, m_id));
    }
}
