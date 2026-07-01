#pragma once

#include <functional>
#include <string>
#include <glm/glm.hpp>

struct GLFWwindow;

namespace RoyalGL
{
    struct WindowDesc
    {
        int width = 1600;
        int height = 900;
        std::string title = "RoyalGL";
        bool vsync = true;
    };

    // Owns the GLFW window, the OpenGL context, and GLEW function loading.
    class Window
    {
    public:
        explicit Window(const WindowDesc& desc);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool ShouldClose() const;
        void PollEvents() const;
        void SwapBuffers() const;

        glm::ivec2 GetFramebufferSize() const;
        GLFWwindow* Handle() const { return m_handle; }

        // Fired whenever the framebuffer is resized (new size in pixels).
        std::function<void(int, int)> OnResize;

        // Fired on mouse wheel scroll (yoffset > 0 = away from user / "up").
        std::function<void(double xoffset, double yoffset)> OnScroll;

    private:
        GLFWwindow* m_handle = nullptr;
    };
}
