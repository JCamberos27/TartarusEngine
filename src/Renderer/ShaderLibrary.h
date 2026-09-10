#pragma once
#include <string>
#include <vector>
#include <functional>

// Reads GLSL source files from the shaders directory (set once at startup via Init) and
// resolves #include "filename" directives recursively before returning the source string.
// Callers pass the returned string directly to Shader's constructor.
namespace ShaderLibrary {
    void Init(const std::string& shadersDir);
    std::string ReadFile(const std::string& filename);

    // Like ReadFile, but throws std::runtime_error naming the full resolved path and the fact
    // that it is a required engine shader when the file is missing or empty — so a wrong-CWD
    // launch fails at startup with "shader not found: <path>" instead of the driver later
    // reporting a misleading "must write to gl_Position" link error (audit #355 / BUG-102).
    std::string ReadFileRequired(const std::string& filename);

    // The directory Init() resolved to (absolute once main() passes an EnginePaths result).
    std::string Dir();

    // Call once per frame (typically in the editor update loop). Polls file modification times
    // at ~4 Hz, invoking `onChanged(filename)` for each .glsl file that changed on disk since
    // the last poll. Does nothing and returns immediately in release/non-editor builds when no
    // listeners are registered. The callback is responsible for hot-reloading (e.g. Shader::Reload).
    void PollForChanges(const std::function<void(const std::string& filename)>& onChanged);
}
