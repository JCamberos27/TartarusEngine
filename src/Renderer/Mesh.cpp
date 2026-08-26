#include "Mesh.h"
#include "gl.h"

Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices) {
    m_IndexCount = static_cast<unsigned int>(indices.size());

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);

    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

    glBindVertexArray(0);
}

Mesh::~Mesh() {
    glDeleteBuffers(1, &m_VBO);
    glDeleteBuffers(1, &m_EBO);
    glDeleteVertexArrays(1, &m_VAO);
}

void Mesh::Draw() const {
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, nullptr);
}

Mesh* Mesh::CreateCube(float size) {
    float h = size * 0.5f;
    std::vector<Vertex> v = {
        // +Z
        {{-h,-h, h}, {0,0,1}}, {{ h,-h, h}, {0,0,1}}, {{ h, h, h}, {0,0,1}}, {{-h, h, h}, {0,0,1}},
        // -Z
        {{ h,-h,-h}, {0,0,-1}}, {{-h,-h,-h}, {0,0,-1}}, {{-h, h,-h}, {0,0,-1}}, {{ h, h,-h}, {0,0,-1}},
        // +X
        {{ h,-h, h}, {1,0,0}}, {{ h,-h,-h}, {1,0,0}}, {{ h, h,-h}, {1,0,0}}, {{ h, h, h}, {1,0,0}},
        // -X
        {{-h,-h,-h}, {-1,0,0}}, {{-h,-h, h}, {-1,0,0}}, {{-h, h, h}, {-1,0,0}}, {{-h, h,-h}, {-1,0,0}},
        // +Y
        {{-h, h, h}, {0,1,0}}, {{ h, h, h}, {0,1,0}}, {{ h, h,-h}, {0,1,0}}, {{-h, h,-h}, {0,1,0}},
        // -Y
        {{-h,-h,-h}, {0,-1,0}}, {{ h,-h,-h}, {0,-1,0}}, {{ h,-h, h}, {0,-1,0}}, {{-h,-h, h}, {0,-1,0}},
    };
    std::vector<unsigned int> idx;
    for (unsigned int f = 0; f < 6; ++f) {
        unsigned int base = f * 4;
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 2); idx.push_back(base + 3); idx.push_back(base + 0);
    }
    return new Mesh(v, idx);
}

Mesh* Mesh::CreatePlane(float size) {
    float h = size * 0.5f;
    std::vector<Vertex> v = {
        {{-h, 0, -h}, {0,1,0}},
        {{ h, 0, -h}, {0,1,0}},
        {{ h, 0,  h}, {0,1,0}},
        {{-h, 0,  h}, {0,1,0}},
    };
    std::vector<unsigned int> idx = {0,1,2, 2,3,0};
    return new Mesh(v, idx);
}
