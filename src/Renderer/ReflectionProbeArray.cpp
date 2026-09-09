#include "ReflectionProbeArray.h"
#include "Shader.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include <glm/gtx/norm.hpp>
#include <algorithm>
#include <array>

void ReflectionProbeArray::Update(const World& world) {
    m_Probes.clear();
    auto view = world.Registry.view<const TransformComponent, const ReflectionProbeComponent>();
    for (auto [entity, tc, rp] : view.each()) {
        glm::mat4 wt = world.GetCachedWorldTransform(entity);
        glm::vec3 center = glm::vec3(wt[3]);
        // Size is in world units; half-extents: multiply by scale from the matrix columns
        glm::vec3 scale = glm::vec3(
            glm::length(glm::vec3(wt[0])),
            glm::length(glm::vec3(wt[1])),
            glm::length(glm::vec3(wt[2])));
        glm::vec3 halfSize = rp.Size * scale * 0.5f;
        m_Probes.push_back({center, halfSize, rp.Importance});
    }
}

void ReflectionProbeArray::Bind(Shader& shader, const glm::vec3& viewCenter) const {
    if (m_Probes.empty()) {
        shader.SetInt("uProbeCount", 0);
        return;
    }

    // Score = importance / (1 + distance^2): closer + more important scores higher.
    struct Scored { int idx; float score; };
    std::array<Scored, kMaxDraw> best;
    int found = 0;

    for (int i = 0; i < (int)m_Probes.size(); ++i) {
        float d2 = glm::length2(m_Probes[i].Center - viewCenter);
        float score = m_Probes[i].Importance / (1.0f + d2);
        if (found < kMaxDraw) {
            best[found++] = {i, score};
            // keep in descending order
            for (int j = found - 1; j > 0 && best[j].score > best[j-1].score; --j)
                std::swap(best[j], best[j-1]);
        } else if (score > best[found-1].score) {
            best[found-1] = {i, score};
            for (int j = found - 1; j > 0 && best[j].score > best[j-1].score; --j)
                std::swap(best[j], best[j-1]);
        }
    }

    // Normalise blend weights so they sum to 1.
    float totalScore = 0.0f;
    for (int j = 0; j < found; ++j) totalScore += best[j].score;
    if (totalScore < 1e-6f) totalScore = 1.0f;

    shader.SetInt("uProbeCount", found);
    for (int j = 0; j < found; ++j) {
        const ProbeData& p = m_Probes[best[j].idx];
        std::string jStr = std::to_string(j);
        shader.SetVec3("uProbeCenter["  + jStr + "]",   p.Center);
        shader.SetVec3("uProbeHalfSize[" + jStr + "]",  p.HalfSize);
        shader.SetFloat("uProbeBlend["  + jStr + "]",   best[j].score / totalScore);
    }
}
