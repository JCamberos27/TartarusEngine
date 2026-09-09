#pragma once
#include <string>

// Reads GLSL source files from the shaders directory (set once at startup via Init) and
// resolves #include "filename" directives recursively before returning the source string.
// Callers pass the returned string directly to Shader's constructor.
namespace ShaderLibrary {
    void Init(const std::string& shadersDir);
    std::string ReadFile(const std::string& filename);
}
