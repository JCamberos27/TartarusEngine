#pragma once
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

class Shader {
public:
    Shader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~Shader();

    void Bind() const;

    void SetMat4(const std::string& name, const glm::mat4& m) const;
    void SetVec3(const std::string& name, const glm::vec3& v) const;
    void SetFloat(const std::string& name, float v) const;
    void SetInt(const std::string& name, int v) const;
    // Uploads `count` consecutive matrices in one driver call — e.g. `SetMat4Array("uBones[0]",
    // 100, bones.data())` for a whole GLSL `uniform mat4 uBones[100]` array, instead of 100
    // separate SetMat4 calls (100 uniform-name lookups + 100 draw-call-adjacent GL calls).
    void SetMat4Array(const std::string& name, int count, const glm::mat4* data) const;

private:
    unsigned int m_Program = 0;
    unsigned int Compile(unsigned int type, const std::string& src);
    // glGetUniformLocation hashes the name string in the driver on every call — cheap in
    // isolation, but adds up when called every frame for every uniform of every draw (and
    // used to be called 100x/frame per animated model just for bone matrices). Cached here
    // since a program's uniform locations never change after linking.
    mutable std::unordered_map<std::string, int> m_UniformCache;
    int Loc(const std::string& name) const;
};
