#include "gfx/Shader.h"
#include "gfx/GLCheck.h"
#include "core/Log.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace RoyalGL
{
    namespace
    {
        std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::in | std::ios::binary);
            if (!file)
                throw std::runtime_error("Shader: failed to open file: " + path.string());
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }

        // Resolves one level of `#include "name"` directives, relative to the
        // including file's own directory. GLSL has no native #include.
        std::string LoadShaderSource(const std::filesystem::path& path)
        {
            std::string source = ReadFile(path);
            std::string result;
            result.reserve(source.size());

            std::istringstream stream(source);
            std::string line;
            while (std::getline(stream, line))
            {
                size_t includePos = line.find("#include");
                if (includePos != std::string::npos)
                {
                    size_t firstQuote = line.find('"', includePos);
                    size_t lastQuote = line.rfind('"');
                    if (firstQuote != std::string::npos && lastQuote != std::string::npos && lastQuote > firstQuote)
                    {
                        std::string includeName = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
                        result += ReadFile(path.parent_path() / includeName);
                        result += '\n';
                        continue;
                    }
                }
                result += line;
                result += '\n';
            }
            return result;
        }

        GLuint CompileStage(GLenum stage, const std::string& source, const std::filesystem::path& debugPath)
        {
            GLuint shader = glCreateShader(stage);
            const char* src = source.c_str();
            glShaderSource(shader, 1, &src, nullptr);
            glCompileShader(shader);

            GLint success = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
            if (!success)
            {
                GLint logLength = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<char> log(static_cast<size_t>(logLength) + 1, '\0');
                glGetShaderInfoLog(shader, logLength, nullptr, log.data());
                glDeleteShader(shader);
                throw std::runtime_error("Shader: failed to compile " + debugPath.string() + ":\n" + log.data());
            }
            return shader;
        }

        GLuint LinkProgram(const std::vector<GLuint>& shaders, const std::filesystem::path& debugPath)
        {
            GLuint program = glCreateProgram();
            for (GLuint s : shaders) glAttachShader(program, s);
            glLinkProgram(program);

            GLint success = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &success);
            if (!success)
            {
                GLint logLength = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<char> log(static_cast<size_t>(logLength) + 1, '\0');
                glGetProgramInfoLog(program, logLength, nullptr, log.data());
                glDeleteProgram(program);
                for (GLuint s : shaders) glDeleteShader(s);
                throw std::runtime_error("Shader: failed to link " + debugPath.string() + ":\n" + log.data());
            }

            for (GLuint s : shaders)
            {
                glDetachShader(program, s);
                glDeleteShader(s);
            }
            return program;
        }
    }

    Shader Shader::CreateCompute(const std::filesystem::path& computePath)
    {
        std::string source = LoadShaderSource(computePath);
        GLuint stage = CompileStage(GL_COMPUTE_SHADER, source, computePath);
        GLuint program = LinkProgram({stage}, computePath);
        ROYALGL_LOG_INFO("Compiled compute shader: ", computePath.string());
        return Shader(program);
    }

    Shader Shader::CreateGraphics(const std::filesystem::path& vertPath, const std::filesystem::path& fragPath)
    {
        std::string vertSource = LoadShaderSource(vertPath);
        std::string fragSource = LoadShaderSource(fragPath);
        GLuint vs = CompileStage(GL_VERTEX_SHADER, vertSource, vertPath);
        GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragSource, fragPath);
        GLuint program = LinkProgram({vs, fs}, vertPath);
        ROYALGL_LOG_INFO("Compiled graphics shader: ", vertPath.string(), " + ", fragPath.string());
        return Shader(program);
    }

    Shader::~Shader()
    {
        if (m_program != 0) glDeleteProgram(m_program);
    }

    Shader::Shader(Shader&& other) noexcept : m_program(other.m_program)
    {
        other.m_program = 0;
    }

    Shader& Shader::operator=(Shader&& other) noexcept
    {
        if (this != &other)
        {
            if (m_program != 0) glDeleteProgram(m_program);
            m_program = other.m_program;
            other.m_program = 0;
        }
        return *this;
    }

    void Shader::Use() const { GL_CALL(glUseProgram(m_program)); }

    void Shader::Dispatch(GLuint groupsX, GLuint groupsY, GLuint groupsZ) const
    {
        GL_CALL(glDispatchCompute(groupsX, groupsY, groupsZ));
    }

    int Shader::UniformLocation(const char* name) const
    {
        return glGetUniformLocation(m_program, name);
    }

    // Uses the DSA glProgramUniform* entry points (core since GL 4.1) so
    // these setters work regardless of which program is currently bound.
    void Shader::SetInt(const char* name, int value) const { glProgramUniform1i(m_program, UniformLocation(name), value); }
    void Shader::SetUint(const char* name, unsigned int value) const { glProgramUniform1ui(m_program, UniformLocation(name), value); }
    void Shader::SetFloat(const char* name, float value) const { glProgramUniform1f(m_program, UniformLocation(name), value); }
    void Shader::SetVec2(const char* name, const glm::vec2& value) const { glProgramUniform2fv(m_program, UniformLocation(name), 1, &value.x); }
    void Shader::SetVec3(const char* name, const glm::vec3& value) const { glProgramUniform3fv(m_program, UniformLocation(name), 1, &value.x); }
    void Shader::SetVec4(const char* name, const glm::vec4& value) const { glProgramUniform4fv(m_program, UniformLocation(name), 1, &value.x); }
    void Shader::SetIVec2(const char* name, const glm::ivec2& value) const { glProgramUniform2iv(m_program, UniformLocation(name), 1, &value.x); }
    void Shader::SetMat4(const char* name, const glm::mat4& value) const { glProgramUniformMatrix4fv(m_program, UniformLocation(name), 1, GL_FALSE, &value[0][0]); }
}
