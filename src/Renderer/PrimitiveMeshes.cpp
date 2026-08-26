#include "PrimitiveMeshes.h"
#include <cmath>
#include <algorithm>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Self-correcting winding: flips a triangle's index order if its geometric face normal
// (from the right-hand rule) disagrees with the analytic vertex normals already stored for
// it. Removes the need to hand-verify CCW/CW winding for every curved primitive by hand —
// the vertex normal (which IS hand-verified per shape) is the single source of truth.
void FixWinding(std::vector<ModelVertex>& verts, std::vector<unsigned int>& indices) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        unsigned int a = indices[i], b = indices[i + 1], c = indices[i + 2];
        glm::vec3 faceNormal = glm::cross(verts[b].Position - verts[a].Position, verts[c].Position - verts[a].Position);
        glm::vec3 avgNormal = verts[a].Normal + verts[b].Normal + verts[c].Normal;
        if (glm::dot(faceNormal, avgNormal) < 0.0f) {
            std::swap(indices[i + 1], indices[i + 2]);
        }
    }
}

ModelVertex MakeVertex(const glm::vec3& pos, const glm::vec3& normal, const glm::vec2& uv, const glm::vec3& tangent) {
    ModelVertex v;
    v.Position = pos;
    v.Normal = normal;
    v.UV = uv;
    v.Tangent = tangent;
    v.TangentSign = 1.0f;
    return v;
}

} // namespace

void PrimitiveMeshes::GenerateCube(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices) {
    float h = 0.5f;
    struct Face { glm::vec3 corners[4]; glm::vec3 normal; glm::vec3 tangent; };
    Face faces[6] = {
        {{{-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}}, {0,0,1},  {1,0,0}},  // +Z
        {{{ h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}}, {0,0,-1}, {-1,0,0}}, // -Z
        {{{ h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}}, {1,0,0},  {0,0,-1}}, // +X
        {{{-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}}, {-1,0,0}, {0,0,1}},  // -X
        {{{-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}}, {0,1,0},  {1,0,0}},  // +Y
        {{{-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}}, {0,-1,0}, {1,0,0}},  // -Y
    };
    glm::vec2 uvs[4] = {{0,0}, {1,0}, {1,1}, {0,1}};

    for (const Face& f : faces) {
        unsigned int base = (unsigned int)vertices.size();
        for (int i = 0; i < 4; ++i) {
            vertices.push_back(MakeVertex(f.corners[i], f.normal, uvs[i], f.tangent));
        }
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 2); indices.push_back(base + 3); indices.push_back(base + 0);
    }
    FixWinding(vertices, indices);
}

void PrimitiveMeshes::GeneratePlane(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices) {
    float h = 0.5f;
    vertices.push_back(MakeVertex({-h, 0, -h}, {0, 1, 0}, {0, 0}, {1, 0, 0}));
    vertices.push_back(MakeVertex({ h, 0, -h}, {0, 1, 0}, {1, 0}, {1, 0, 0}));
    vertices.push_back(MakeVertex({ h, 0,  h}, {0, 1, 0}, {1, 1}, {1, 0, 0}));
    vertices.push_back(MakeVertex({-h, 0,  h}, {0, 1, 0}, {0, 1}, {1, 0, 0}));
    indices = {0, 1, 2, 2, 3, 0};
    FixWinding(vertices, indices);
}

void PrimitiveMeshes::GenerateSphere(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int latSegments, int lonSegments) {
    const float radius = 0.5f;
    int cols = lonSegments + 1;

    for (int lat = 0; lat <= latSegments; ++lat) {
        float theta = (float)lat * kPi / (float)latSegments; // 0 (north pole) .. PI (south pole)
        float sinTheta = std::sin(theta), cosTheta = std::cos(theta);
        for (int lon = 0; lon <= lonSegments; ++lon) {
            float phi = (float)lon * 2.0f * kPi / (float)lonSegments;
            float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
            glm::vec3 dir(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
            glm::vec3 tangent = glm::normalize(glm::vec3(-sinPhi, 0.0f, cosPhi));
            vertices.push_back(MakeVertex(dir * radius, dir, {(float)lon / lonSegments, (float)lat / latSegments}, tangent));
        }
    }

    for (int lat = 0; lat < latSegments; ++lat) {
        for (int lon = 0; lon < lonSegments; ++lon) {
            unsigned int i0 = lat * cols + lon;
            unsigned int i1 = i0 + 1;
            unsigned int i2 = i0 + cols;
            unsigned int i3 = i2 + 1;
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }
    FixWinding(vertices, indices);
}

void PrimitiveMeshes::GenerateCylinder(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int segments) {
    const float radius = 0.5f, halfHeight = 0.5f;
    int cols = segments + 1;

    // Side surface: bottom ring then top ring, radial normals.
    for (int ring = 0; ring < 2; ++ring) {
        float y = ring == 0 ? -halfHeight : halfHeight;
        for (int seg = 0; seg <= segments; ++seg) {
            float phi = (float)seg * 2.0f * kPi / (float)segments;
            float x = radius * std::cos(phi), z = radius * std::sin(phi);
            glm::vec3 normal = glm::normalize(glm::vec3(x, 0.0f, z));
            glm::vec3 tangent = glm::normalize(glm::vec3(-std::sin(phi), 0.0f, std::cos(phi)));
            vertices.push_back(MakeVertex({x, y, z}, normal, {(float)seg / segments, (float)ring}, tangent));
        }
    }
    for (int seg = 0; seg < segments; ++seg) {
        unsigned int i0 = seg, i1 = seg + 1, i2 = cols + seg, i3 = cols + seg + 1;
        indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
        indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
    }

    // Caps: duplicated vertices (flat normals) plus a center vertex, fanned around.
    auto addCap = [&](float y, const glm::vec3& normal) {
        unsigned int center = (unsigned int)vertices.size();
        vertices.push_back(MakeVertex({0, y, 0}, normal, {0.5f, 0.5f}, {1, 0, 0}));
        unsigned int ringStart = (unsigned int)vertices.size();
        for (int seg = 0; seg <= segments; ++seg) {
            float phi = (float)seg * 2.0f * kPi / (float)segments;
            float x = radius * std::cos(phi), z = radius * std::sin(phi);
            glm::vec2 uv(0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi));
            vertices.push_back(MakeVertex({x, y, z}, normal, uv, {1, 0, 0}));
        }
        for (int seg = 0; seg < segments; ++seg) {
            indices.push_back(center);
            indices.push_back(ringStart + seg);
            indices.push_back(ringStart + seg + 1);
        }
    };
    addCap(-halfHeight, {0, -1, 0});
    addCap(halfHeight, {0, 1, 0});

    FixWinding(vertices, indices);
}

void PrimitiveMeshes::GenerateCone(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int segments) {
    const float radius = 0.5f, halfHeight = 0.5f, height = 1.0f;

    // Apex shared by every side triangle — an imperceptible simplification at a single point.
    unsigned int apex = (unsigned int)vertices.size();
    vertices.push_back(MakeVertex({0, halfHeight, 0}, {0, 1, 0}, {0.5f, 0.0f}, {1, 0, 0}));

    unsigned int baseStart = (unsigned int)vertices.size();
    for (int seg = 0; seg <= segments; ++seg) {
        float phi = (float)seg * 2.0f * kPi / (float)segments;
        float cosPhi = std::cos(phi), sinPhi = std::sin(phi);
        float x = radius * cosPhi, z = radius * sinPhi;
        // Standard cone lateral-surface normal: outward and tilted toward the apex side.
        glm::vec3 normal = glm::normalize(glm::vec3(cosPhi, radius / height, sinPhi));
        glm::vec3 tangent = glm::normalize(glm::vec3(-sinPhi, 0.0f, cosPhi));
        vertices.push_back(MakeVertex({x, -halfHeight, z}, normal, {(float)seg / segments, 1.0f}, tangent));
    }
    for (int seg = 0; seg < segments; ++seg) {
        indices.push_back(apex);
        indices.push_back(baseStart + seg);
        indices.push_back(baseStart + seg + 1);
    }

    // Base cap.
    unsigned int capCenter = (unsigned int)vertices.size();
    vertices.push_back(MakeVertex({0, -halfHeight, 0}, {0, -1, 0}, {0.5f, 0.5f}, {1, 0, 0}));
    unsigned int capRingStart = (unsigned int)vertices.size();
    for (int seg = 0; seg <= segments; ++seg) {
        float phi = (float)seg * 2.0f * kPi / (float)segments;
        float x = radius * std::cos(phi), z = radius * std::sin(phi);
        glm::vec2 uv(0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi));
        vertices.push_back(MakeVertex({x, -halfHeight, z}, {0, -1, 0}, uv, {1, 0, 0}));
    }
    for (int seg = 0; seg < segments; ++seg) {
        indices.push_back(capCenter);
        indices.push_back(capRingStart + seg);
        indices.push_back(capRingStart + seg + 1);
    }

    FixWinding(vertices, indices);
}
