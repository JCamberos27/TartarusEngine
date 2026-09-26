#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLDebug.h"
#include "Log.h"
#include "gl.h"
#include "GLStateCache.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <iostream>

namespace {
// GL-101/#367 diagnostic: give the linked program a KHR_debug label and, when the driver's
// debug output is live, log its id -> name so a "program N" message identifies itself. Loaded
// via glfwGetProcAddress like GLDebug.cpp does — deliberately not added to the hand-rolled
// gl.h loader. No-op when the context has no KHR_debug.
void TagProgram(unsigned int program, const char* debugName) {
    if (!debugName || program == 0) return;
    constexpr GLenum kProgramObject = 0x82E2; // GL_PROGRAM
    using PFN_glObjectLabel = void(__stdcall*)(GLenum, GLuint, GLsizei, const GLchar*);
    if (auto objectLabel = reinterpret_cast<PFN_glObjectLabel>(glfwGetProcAddress("glObjectLabel")))
        objectLabel(kProgramObject, program, -1, debugName);
    if (GLDebug::IsEnabled())
        Log::Info(std::string("[GL] program ") + std::to_string(program) + " = " + debugName);
}
// #158 - full info logs (they used to be cut at 1024 chars, losing every error after the first
// few), with source-string numbers mapped back to file names.
std::string ProgramLog(unsigned int program) {
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    std::string log((size_t)std::max(len, 1), '\0');
    glGetProgramInfoLog(program, (GLsizei)log.size(), nullptr, log.data());
    log.resize(std::strlen(log.c_str()));
    return ShaderLibrary::AnnotateLog(log);
}
std::string ShaderLog(unsigned int shader) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::string log((size_t)std::max(len, 1), '\0');
    glGetShaderInfoLog(shader, (GLsizei)log.size(), nullptr, log.data());
    log.resize(std::strlen(log.c_str()));
    return ShaderLibrary::AnnotateLog(log);
}
} // namespace

Shader::Shader(const std::string& vertexSrc, const std::string& fragmentSrc, const char* debugName) {
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
        const std::string log = ProgramLog(m_Program);
        glDeleteProgram(m_Program);
        m_Program = 0;
        throw std::runtime_error(std::string("Shader link error: ") + log);
    }
    TagProgram(m_Program, debugName);
}

Shader::Shader(const std::string& computeSrc, const char* debugName) {
    unsigned int cs = Compile(GL_COMPUTE_SHADER, computeSrc);

    m_Program = glCreateProgram();
    glAttachShader(m_Program, cs);
    glLinkProgram(m_Program);
    glDeleteShader(cs);

    int success;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &success);
    if (!success) {
        const std::string log = ProgramLog(m_Program);
        glDeleteProgram(m_Program);
        m_Program = 0;
        throw std::runtime_error(std::string("Compute shader link error: ") + log);
    }
    TagProgram(m_Program, debugName);
}

Shader::~Shader() {
    glDeleteProgram(m_Program);
}

void Shader::Reload(const std::string& vertFile, const std::string& fragFile) {
    std::string vertSrc = ShaderLibrary::ReadFile(vertFile);
    std::string fragSrc = ShaderLibrary::ReadFile(fragFile);

    unsigned int vs = Compile(GL_VERTEX_SHADER, vertSrc);
    unsigned int fs = 0;
    try {
        fs = Compile(GL_FRAGMENT_SHADER, fragSrc);
    } catch (...) {
        glDeleteShader(vs);
        throw;
    }

    unsigned int newProg = glCreateProgram();
    glAttachShader(newProg, vs);
    glAttachShader(newProg, fs);
    glLinkProgram(newProg);
    glDeleteShader(vs);
    glDeleteShader(fs);

    int success;
    glGetProgramiv(newProg, GL_LINK_STATUS, &success);
    if (!success) {
        const std::string log = ProgramLog(newProg);
        glDeleteProgram(newProg);
        throw std::runtime_error(std::string("Shader link error: ") + log);
    }

    glDeleteProgram(m_Program);
    m_Program = newProg;
    m_UniformCache.clear();
    m_LocationBlock.reset();
}

void Shader::DispatchCompute(unsigned int gx, unsigned int gy, unsigned int gz) const {
    GLStateCache::UseProgram(m_Program);
    glDispatchCompute(gx, gy, gz);
}

unsigned int Shader::Compile(unsigned int type, const std::string& src) {
    unsigned int shader = glCreateShader(type);
    const char* csrc = src.c_str();
    glShaderSource(shader, 1, &csrc, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        const std::string log = ShaderLog(shader);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compile error: ") + log);
    }
    return shader;
}

void Shader::Bind() const {
    GLStateCache::UseProgram(m_Program);
}

int Shader::Loc(std::string_view name) const {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    for (const char c : name) { h ^= (unsigned char)c; h *= 1099511628211ull; }
    auto it = m_UniformCache.find(h);
    if (it != m_UniformCache.end() && it->second.Name == name) return it->second.Loc;
    std::string owned(name);
    const int loc = glGetUniformLocation(m_Program, owned.c_str());
    if (it == m_UniformCache.end()) m_UniformCache.emplace(h, CachedUniform{std::move(owned), loc});
    return loc;
}

void Shader::SetMat4(std::string_view name, const glm::mat4& m) const {
    glUniformMatrix4fv(Loc(name), 1, GL_FALSE, glm::value_ptr(m));
}

void Shader::SetMat4Array(std::string_view name, int count, const glm::mat4* data) const {
    glUniformMatrix4fv(Loc(name), count, GL_FALSE, glm::value_ptr(data[0]));
}

void Shader::SetVec3Array(std::string_view name, int count, const glm::vec3* data) const {
    if (count > 0) glUniform3fv(Loc(name), count, glm::value_ptr(data[0]));
}

void Shader::SetFloatArray(std::string_view name, int count, const float* data) const {
    if (count > 0) glUniform1fv(Loc(name), count, data);
}

void Shader::SetVec2(std::string_view name, const glm::vec2& v) const {
    glUniform2f(Loc(name), v.x, v.y);
}

void Shader::SetVec3(std::string_view name, const glm::vec3& v) const {
    glUniform3f(Loc(name), v.x, v.y, v.z);
}

void Shader::SetVec4(std::string_view name, const glm::vec4& v) const {
    glUniform4f(Loc(name), v.x, v.y, v.z, v.w);
}

void Shader::SetFloat(std::string_view name, float v) const {
    glUniform1f(Loc(name), v);
}

void Shader::SetInt(std::string_view name, int v) const {
    glUniform1i(Loc(name), v);
}

// --- Int-location overloads (#194): same GL calls, no name lookup — the caller resolves the
// location once (via Loc()) outside the hot loop instead of on every iteration.
void Shader::SetMat4(int loc, const glm::mat4& m) const {
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(m));
}

void Shader::SetVec2(int loc, const glm::vec2& v) const {
    glUniform2f(loc, v.x, v.y);
}

void Shader::SetVec3(int loc, const glm::vec3& v) const {
    glUniform3f(loc, v.x, v.y, v.z);
}

void Shader::SetVec4(int loc, const glm::vec4& v) const {
    glUniform4f(loc, v.x, v.y, v.z, v.w);
}

void Shader::SetFloat(int loc, float v) const {
    glUniform1f(loc, v);
}

void Shader::SetInt(int loc, int v) const {
    glUniform1i(loc, v);
}
