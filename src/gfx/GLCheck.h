#pragma once

#include <GL/glew.h>
#include <cstdio>

// Debug-only OpenGL error checking. Wrap individual GL calls:
//   GL_CALL(glBindBuffer(GL_ARRAY_BUFFER, buf));
// In release (NDEBUG) builds this compiles down to just `x`.
namespace RoyalGL
{
    inline void CheckGLError(const char* file, int line, const char* expr)
    {
        GLenum err;
        while ((err = glGetError()) != GL_NO_ERROR)
        {
            const char* msg = "UNKNOWN_GL_ERROR";
            switch (err)
            {
                case GL_INVALID_ENUM: msg = "GL_INVALID_ENUM"; break;
                case GL_INVALID_VALUE: msg = "GL_INVALID_VALUE"; break;
                case GL_INVALID_OPERATION: msg = "GL_INVALID_OPERATION"; break;
                case GL_INVALID_FRAMEBUFFER_OPERATION: msg = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
                case GL_OUT_OF_MEMORY: msg = "GL_OUT_OF_MEMORY"; break;
                case GL_STACK_UNDERFLOW: msg = "GL_STACK_UNDERFLOW"; break;
                case GL_STACK_OVERFLOW: msg = "GL_STACK_OVERFLOW"; break;
                default: break;
            }
            std::fprintf(stderr, "[GL ERROR] %s (%s:%d) evaluating `%s`\n", msg, file, line, expr);
        }
    }
}

#ifdef NDEBUG
#define GL_CALL(x) x
#else
#define GL_CALL(x) do { x; ::RoyalGL::CheckGLError(__FILE__, __LINE__, #x); } while (0)
#endif
