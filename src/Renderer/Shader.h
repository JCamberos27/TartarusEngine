#pragma once
#include <string>
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

private:
    unsigned int m_Program = 0;
    unsigned int Compile(unsigned int type, const std::string& src);
    int Loc(const std::string& name) const;
};
