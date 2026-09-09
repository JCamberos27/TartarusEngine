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

    // Call once per frame (typically in the editor update loop). Polls file modification times
    // at ~4 Hz, invoking `onChanged(filename)` for each .glsl file that changed on disk since
    // the last poll. Does nothing and returns immediately in release/non-editor builds when no
    // listeners are registered. The callback is responsible for hot-reloading (e.g. Shader::Reload).
    void PollForChanges(const std::function<void(const std::string& filename)>& onChanged);
}
