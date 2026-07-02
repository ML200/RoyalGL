#include "core/Window.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "core/Log.h"

#include <cstdlib>
#include <stdexcept>

namespace RoyalGL
{
    namespace
    {
        void GlfwErrorCallback(int error, const char* description)
        {
            ROYALGL_LOG_ERROR("GLFW error (", error, "): ", description);
        }

        // KHR_debug sink (ROYALGL_GL_DEBUG=1): forwards driver messages to
        // the log. Notifications are dropped; NVIDIA emits one per buffer
        // placement.
        void GLAPIENTRY GlDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                        GLsizei, const GLchar* message, const void*)
        {
            (void)source;
            if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
            if (severity == GL_DEBUG_SEVERITY_HIGH || type == GL_DEBUG_TYPE_ERROR)
                ROYALGL_LOG_ERROR("GL debug (", id, "): ", message);
            else
                ROYALGL_LOG_WARN("GL debug (", id, "): ", message);
        }

        void FramebufferSizeCallback(GLFWwindow* handle, int width, int height)
        {
            auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle));
            if (window->OnResize)
                window->OnResize(width, height);
        }

        void ScrollCallback(GLFWwindow* handle, double xoffset, double yoffset)
        {
            auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle));
            if (window->OnScroll)
                window->OnScroll(xoffset, yoffset);
        }
    }

    Window::Window(const WindowDesc& desc)
    {
        glfwSetErrorCallback(GlfwErrorCallback);

        if (!glfwInit())
            throw std::runtime_error("Window: glfwInit failed");

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

        // ROYALGL_GL_DEBUG=1: debug context + message callback for
        // Nsight/RenderDoc sessions and driver diagnostics. Object labels
        // and per-pass debug groups are always on (they cost nothing
        // without a tool attached).
        bool glDebug = (std::getenv("ROYALGL_GL_DEBUG") != nullptr);
        if (glDebug)
            glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        m_handle = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
        if (!m_handle)
        {
            glfwTerminate();
            throw std::runtime_error("Window: glfwCreateWindow failed");
        }

        glfwMakeContextCurrent(m_handle);
        glfwSwapInterval(desc.vsync ? 1 : 0);

        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK)
            throw std::runtime_error("Window: glewInit failed");

        ROYALGL_LOG_INFO("GL_VERSION: ", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        ROYALGL_LOG_INFO("GL_RENDERER: ", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

        if (glDebug)
        {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS); // messages on the offending call's stack
            glDebugMessageCallback(GlDebugCallback, nullptr);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
            ROYALGL_LOG_INFO("GL debug context enabled (ROYALGL_GL_DEBUG)");
        }

        glfwSetWindowUserPointer(m_handle, this);
        glfwSetFramebufferSizeCallback(m_handle, FramebufferSizeCallback);
        glfwSetScrollCallback(m_handle, ScrollCallback);
    }

    Window::~Window()
    {
        if (m_handle)
            glfwDestroyWindow(m_handle);
        glfwTerminate();
    }

    bool Window::ShouldClose() const
    {
        return glfwWindowShouldClose(m_handle) != 0;
    }

    void Window::PollEvents() const
    {
        glfwPollEvents();
    }

    void Window::SwapBuffers() const
    {
        glfwSwapBuffers(m_handle);
    }

    glm::ivec2 Window::GetFramebufferSize() const
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(m_handle, &w, &h);
        return glm::ivec2(w, h);
    }
}
