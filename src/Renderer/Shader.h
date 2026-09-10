#pragma once
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

class Shader {
public:
    // debugName (optional): tags the linked GL program via glObjectLabel and, when GL debug
    // output is active, logs "program <id> = <name>" at link time — so a driver KHR_debug
    // message that only names a bare program number (e.g. 131218 "vertex shader in program 12
    // is being recompiled") can be mapped back to a concrete shader (audit GL-102 / #367).
    Shader(const std::string& vertexSrc, const std::string& fragmentSrc, const char* debugName = nullptr);
    // Compute-only program (single GL_COMPUTE_SHADER stage) — clustered light culling (#120).
    explicit Shader(const std::string& computeSrc, const char* debugName = nullptr);
    ~Shader();

    // Owns a raw GL program name that ~Shader() glDeleteProgram's, so a copy or move would
    // double-free it (Framebuffer is = delete for the identical reason). Nothing copies a
    // Shader today — ShaderAsset holds it by unique_ptr — but make the mistake a compile error.
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) = delete;
    Shader& operator=(Shader&&) = delete;

    void Bind() const;

    // The raw GL program name. Used as half the key for the material-bind dedup (#192) — a
    // cached "this material is already bound" is only valid while the same program is current.
    unsigned int Program() const { return m_Program; }

    // Dispatches this (compute) program over an x*y*z grid of work groups.
    void DispatchCompute(unsigned int groupsX, unsigned int groupsY, unsigned int groupsZ) const;

    void SetMat4(const std::string& name, const glm::mat4& m) const;
    void SetVec2(const std::string& name, const glm::vec2& v) const;
    void SetVec3(const std::string& name, const glm::vec3& v) const;
    void SetVec4(const std::string& name, const glm::vec4& v) const;
    void SetFloat(const std::string& name, float v) const;
    void SetInt(const std::string& name, int v) const;
    // Uploads `count` consecutive matrices in one driver call — e.g. `SetMat4Array("uBones[0]",
    // 100, bones.data())` for a whole GLSL `uniform mat4 uBones[100]` array, instead of 100
    // separate SetMat4 calls (100 uniform-name lookups + 100 draw-call-adjacent GL calls).
    void SetMat4Array(const std::string& name, int count, const glm::mat4* data) const;

    // Re-reads vertFile and fragFile via ShaderLibrary, recompiles and relinks the program in
    // place, and clears the uniform location cache. Throws on compile/link failure (the old
    // program remains bound and valid in that case).
    void Reload(const std::string& vertFile, const std::string& fragFile);

    // Resolves (and caches) a uniform's location by name — call this ONCE outside a hot loop,
    // then use the int-location overloads below inside it. Avoids re-constructing and re-hashing
    // the name string on every iteration (#194: per-caster shadow loops, main draw loop).
    int Loc(const std::string& name) const;
    void SetMat4(int loc, const glm::mat4& m) const;
    void SetVec2(int loc, const glm::vec2& v) const;
    void SetVec3(int loc, const glm::vec3& v) const;
    void SetVec4(int loc, const glm::vec4& v) const;
    void SetFloat(int loc, float v) const;
    void SetInt(int loc, int v) const;

private:
    unsigned int m_Program = 0;
    unsigned int Compile(unsigned int type, const std::string& src);
    // glGetUniformLocation hashes the name string in the driver on every call — cheap in
    // isolation, but adds up when called every frame for every uniform of every draw (and
    // used to be called 100x/frame per animated model just for bone matrices). Cached here
    // since a program's uniform locations never change after linking.
    mutable std::unordered_map<std::string, int> m_UniformCache;
};
