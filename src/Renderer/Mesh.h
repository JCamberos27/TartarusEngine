#pragma once
#include <vector>
#include <glm/glm.hpp>

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
};

// A single indexed vertex/normal mesh. Drawn as GL_TRIANGLES.
class Mesh {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
    ~Mesh();

    void Draw() const;

    static Mesh* CreateCube(float size = 1.0f);
    static Mesh* CreatePlane(float size = 1.0f);

private:
    unsigned int m_VAO = 0, m_VBO = 0, m_EBO = 0;
    unsigned int m_IndexCount = 0;
};
