#include "Shader.h"
#include "gl.h"
#include "GLStateCache.h"
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <vector>
#include <iostream>

Shader::Shader(const std::string& vertexSrc, const std::string& fragmentSrc) {
    unsigned int vs = Compile(GL_VERTEX_SHADER, vertexSrc);
    unsigned int fs = 0;
    try {
        fs = Compile(GL_FRAGMENT_SHADER, fragmentSrc);
    } catch (...) {
        glDeleteShader(vs); // the fragment stage failed to compile — don't leak the vertex one
        throw;
    }

    m_Program = glCreateProgram();
    glAttachShader(m_Program, vs);
    glAttachShader(m_Program, fs);
    glLinkProgram(m_Program);
    // Flagged for deletion now; the driver frees them once they're detached at link time.
    // Doing it here (not after the status check) means a link failure below can't leak them.
    glDeleteShader(vs);
    glDeleteShader(fs);

    int success;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(m_Program, 1024, nullptr, log);
        glDeleteProgram(m_Program);
        m_Program = 0;
        throw std::runtime_error(std::string("Shader link error: ") + log);
    }
}

Shader::~Shader() {
    glDeleteProgram(m_Program);
}

unsigned int Shader::Compile(unsigned int type, const std::string& src) {
    unsigned int shader = glCreateShader(type);
    const char* csrc = src.c_str();
    glShaderSource(shader, 1, &csrc, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(shader, 1024, nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compile error: ") + log);
    }
    return shader;
}

void Shader::Bind() const {
    GLStateCache::UseProgram(m_Program);
}

int Shader::Loc(const std::string& name) const {
    auto it = m_UniformCache.find(name);
    if (it != m_UniformCache.end()) return it->second;
    int loc = glGetUniformLocation(m_Program, name.c_str());
    m_UniformCache.emplace(name, loc);
    return loc;
}

void Shader::SetMat4(const std::string& name, const glm::mat4& m) const {
    glUniformMatrix4fv(Loc(name), 1, GL_FALSE, glm::value_ptr(m));
}

void Shader::SetMat4Array(const std::string& name, int count, const glm::mat4* data) const {
    glUniformMatrix4fv(Loc(name), count, GL_FALSE, glm::value_ptr(data[0]));
}

void Shader::SetVec3(const std::string& name, const glm::vec3& v) const {
    glUniform3f(Loc(name), v.x, v.y, v.z);
}

void Shader::SetVec4(const std::string& name, const glm::vec4& v) const {
    glUniform4f(Loc(name), v.x, v.y, v.z, v.w);
}

void Shader::SetFloat(const std::string& name, float v) const {
    glUniform1f(Loc(name), v);
}

void Shader::SetInt(const std::string& name, int v) const {
    glUniform1i(Loc(name), v);
}
