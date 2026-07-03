#pragma once

#include <filesystem>
#include <glm/glm.hpp>
#include <GL/glew.h>

namespace RoyalGL
{
    // Thin wrapper around a linked GLSL program (either a single compute
    // shader, or a vertex+fragment pair). Resource bindings (UBOs, SSBOs,
    // images, samplers) use explicit `layout(binding = N)` in GLSL, so this
    // class only deals with loose scalar/vector uniforms.
    class Shader
    {
    public:
        static Shader CreateCompute(const std::filesystem::path& computePath);
        static Shader CreateGraphics(const std::filesystem::path& vertPath, const std::filesystem::path& fragPath);

        Shader() = default;
        ~Shader();

        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;
        Shader(Shader&& other) noexcept;
        Shader& operator=(Shader&& other) noexcept;

        void Use() const;
        void Dispatch(GLuint groupsX, GLuint groupsY, GLuint groupsZ = 1) const;
        // GPU-driven dispatch: reads (groupsX, groupsY, groupsZ) from the
        // buffer bound to GL_DISPATCH_INDIRECT_BUFFER at the given byte
        // offset (wavefront ReSTIR, see shaders/restir_wf_common.glsl).
        void DispatchIndirect(GLintptr offset) const;

        void SetInt(const char* name, int value) const;
        void SetUint(const char* name, unsigned int value) const;
        void SetFloat(const char* name, float value) const;
        void SetVec2(const char* name, const glm::vec2& value) const;
        void SetVec3(const char* name, const glm::vec3& value) const;
        void SetVec4(const char* name, const glm::vec4& value) const;
        void SetIVec2(const char* name, const glm::ivec2& value) const;
        void SetMat4(const char* name, const glm::mat4& value) const;

        bool IsValid() const { return m_program != 0; }
        GLuint Id() const { return m_program; }

    private:
        explicit Shader(GLuint program) : m_program(program) {}
        int UniformLocation(const char* name) const;

        GLuint m_program = 0;
    };
}
