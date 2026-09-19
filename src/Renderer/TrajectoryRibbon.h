#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Shader;

// World-space polylines drawn as camera-facing ribbons of constant on-screen width, plus flat
// rings lying on surfaces - the gravity gun's predicted throw arc, its bounces and where it comes
// to rest (a plain GL line is one pixel wide and gets lost). Drawn into the HDR scene target after
// the scene, depth-tested (walls hide the far part of the arc) with no depth write, alpha-blended.
// Reuses the ColliderGizmo line program (pos.xyz + rgba per vertex).
//
// Usage per frame: AddPath / AddRing any number of times, then Draw (which clears the batch).
class TrajectoryRibbon {
public:
    TrajectoryRibbon();
    ~TrajectoryRibbon();
    TrajectoryRibbon(const TrajectoryRibbon&) = delete;
    TrajectoryRibbon& operator=(const TrajectoryRibbon&) = delete;

    // `fadeInSegments` > 0 fades the first few segments in (so the arc doesn't start as a blob
    // right in front of the held object). `color` is linear HDR, alpha = opacity.
    void AddPath(const std::vector<glm::vec3>& points, const glm::vec4& color, int fadeInSegments = 0);
    void AddRing(const glm::vec3& center, const glm::vec3& normal, float radius, const glm::vec4& color);
    void Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye);

private:
    struct Path { std::vector<glm::vec3> Points; glm::vec4 Color; int FadeIn; };
    struct Ring { glm::vec3 Center, Normal; float Radius; glm::vec4 Color; };
    std::vector<Path> m_Paths;
    std::vector<Ring> m_Rings;

    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // floats
    std::vector<float> m_Verts;
};
