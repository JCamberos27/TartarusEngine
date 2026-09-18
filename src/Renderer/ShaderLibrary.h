#pragma once
#include <string>
#include <vector>
#include <functional>

class Shader;

// Reads GLSL source files from the shaders directory (set once at startup via Init) and
// resolves #include "filename" directives recursively before returning the source string.
// Callers pass the returned string directly to Shader's constructor.
namespace ShaderLibrary {
    void Init(const std::string& shadersDir);
    // `deps` (optional) receives every file read - the file itself and each #include - as
    // DependencyKey()s, for hot reload (#158).
    std::string ReadFile(const std::string& filename, std::vector<std::string>* deps = nullptr);

    // Like ReadFile, but throws std::runtime_error naming the full resolved path and the fact
    // that it is a required engine shader when the file is missing or empty — so a wrong-CWD
    // launch fails at startup with "shader not found: <path>" instead of the driver later
    // reporting a misleading "must write to gl_Position" link error (audit #355 / BUG-102).
    std::string ReadFileRequired(const std::string& filename);

    // The directory Init() resolved to (absolute once main() passes an EnginePaths result).
    std::string Dir();

    // #208 — reads a shader source at an explicit path (a project shader's stage file). Its
    // #includes resolve next to the including file first, then in the engine shader directory,
    // so a project shader can ship its own includes and still use the engine's (Lighting.glsl).
    // Returns "" (and logs, naming `referencedBy`) when the file can't be read.
    std::string ReadFileAt(const std::string& path, const std::string& referencedBy = {},
                           std::vector<std::string>* deps = nullptr);

    // #158 — the canonical spelling of a shader file path used for dependency matching
    // (normalised, forward slashes, lower-case on Windows).
    std::string DependencyKey(const std::string& path);

    // #208 — resolves a shader file reference to an absolute path:
    //   "engine://Name.glsl"   the engine shader directory
    //   "project://dir/x.glsl" the project root
    //   absolute path          itself
    //   "Name.glsl" (relative) next to `baseDir` (the referencing descriptor) when it exists
    //                          there, otherwise the engine shader directory - which keeps every
    //                          descriptor written before namespaces existed working.
    std::string ResolveRef(const std::string& ref, const std::string& baseDir);

    // #158 - ReadFile emits `#line N <fileIndex>` around every #include (and after #version), so
    // a driver error's "<fileIndex>(<line>)" / "<fileIndex>:<line>" points into the right file.
    // AnnotateLog rewrites those prefixes to "File.glsl(line)". Index 0 is any source that didn't
    // come through ReadFile.
    std::string AnnotateLog(const std::string& log);

    // #158 — shader hot reload, editor only (main.cpp never calls it for headless runs).
    // Call once per frame; at most ~4 times a second it scans the engine shader directory and
    // `extraRoots` (project/shaders) RECURSIVELY for .glsl/.vert/.frag/.comp/.shader files and
    // returns the DependencyKey()s of the ones whose modification time changed since the last
    // scan (a file's first sighting is not a change). Usually empty.
    std::vector<std::string> PollChangedFiles(const std::vector<std::string>& extraRoots);

    // Engine programs built from ReadFile names (main.cpp's modelShader etc.) register here;
    // ReloadChanged recompiles every registered program that read any of `changed` (directly or
    // through an #include), keeping the old program if the new source fails to compile, and
    // returns how many it reloaded. Unregister before the Shader is destroyed.
    void RegisterForHotReload(Shader& shader, const std::string& vertFile, const std::string& fragFile);
    void UnregisterForHotReload(Shader& shader);
    void ClearHotReload(); // drops every registration (shutdown / scope exit of the programs)
    int  ReloadChanged(const std::vector<std::string>& changed);
}
