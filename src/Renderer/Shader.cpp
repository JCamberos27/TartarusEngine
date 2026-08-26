#include "Shader.h"
#include "gl.h"
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <vector>
#include <iostream>

Shader::Shader(const std::string& vertexSrc, const std::string& fragmentSrc) {
    unsigned int vs = Compile(GL_VERTEX_SHADER, vertexSrc);
    unsigned int fs = Compile(GL_FRAGMENT_SHADER, fragmentSrc);

    m_Program = glCreateProgram();
    glAttachShader(m_Program, vs);
    glAttachShader(m_Program, fs);
    glLinkProgram(m_Program);

    int success;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(m_Program, 1024, nullptr, log);
        throw std::runtime_error(std::string("Shader link error: ") + log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
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
        throw std::runtime_error(std::string("Shader compile error: ") + log);
    }
    return shader;
}

void Shader::Bind() const {
    glUseProgram(m_Program);
}

int Shader::Loc(const std::string& name) const {
    return glGetUniformLocation(m_Program, name.c_str());
}

void Shader::SetMat4(const std::string& name, const glm::mat4& m) const {
    glUniformMatrix4fv(Loc(name), 1, GL_FALSE, glm::value_ptr(m));
}

void Shader::SetVec3(const std::string& name, const glm::vec3& v) const {
    glUniform3f(Loc(name), v.x, v.y, v.z);
}

void Shader::SetFloat(const std::string& name, float v) const {
    glUniform1f(Loc(name), v);
}

void Shader::SetInt(const std::string& name, int v) const {
    glUniform1i(Loc(name), v);
}
