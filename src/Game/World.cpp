#include "World.h"
#include <glm/gtc/matrix_transform.hpp>

World::World() {
    // Ground
    Boxes.push_back({{0, -0.5f, 0}, {60.0f, 1.0f, 60.0f}, {0.25f, 0.28f, 0.25f}});

    // A scattering of crate-like obstacles / targets.
    struct P { float x, z, s, h; };
    P layout[] = {
        {5, 5, 2, 2}, {-6, 4, 2, 3}, {8, -4, 2, 1.5f}, {-4, -6, 2, 2.5f},
        {0, 10, 2, 2}, {12, 0, 2, 4}, {-12, 0, 2, 2}, {3, -10, 2, 2},
        {-8, -8, 1.5f, 1.5f}, {10, 8, 2, 2},
    };
    glm::vec3 colors[] = {
        {0.8f,0.3f,0.2f}, {0.2f,0.5f,0.8f}, {0.8f,0.7f,0.2f}, {0.4f,0.8f,0.3f},
        {0.7f,0.3f,0.7f}, {0.3f,0.8f,0.8f}, {0.9f,0.5f,0.2f}, {0.5f,0.5f,0.9f},
        {0.9f,0.2f,0.4f}, {0.4f,0.9f,0.6f},
    };
    for (int i = 0; i < 10; ++i) {
        const P& p = layout[i];
        Boxes.push_back({{p.x, p.h * 0.5f, p.z}, {p.s, p.h, p.s}, colors[i]});
    }
}

bool World::ResolveCollisions(AABB& mover, glm::vec3& velocity) const {
    bool grounded = false;
    for (const auto& box : Boxes) {
        if (!box.Alive) continue;
        AABB b = box.Bounds();
        if (mover.Intersects(b)) {
            glm::vec3 mtv = mover.MTV(b);
            mover.Min += mtv;
            mover.Max += mtv;
            if (mtv.y > 0.0f) {
                grounded = true;
                if (velocity.y < 0.0f) velocity.y = 0.0f;
            } else if (mtv.y < 0.0f) {
                if (velocity.y > 0.0f) velocity.y = 0.0f;
            }
            if (mtv.x != 0.0f) velocity.x = 0.0f;
            if (mtv.z != 0.0f) velocity.z = 0.0f;
        }
    }
    return grounded;
}

int World::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float& outDist) const {
    int best = -1;
    float bestT = maxDist;
    for (int i = 0; i < (int)Boxes.size(); ++i) {
        if (!Boxes[i].Alive) continue;
        float t;
        if (Boxes[i].Bounds().RayIntersect(origin, dir, t) && t < bestT) {
            bestT = t;
            best = i;
        }
    }
    outDist = bestT;
    return best;
}

glm::mat4 ComposeTransform(const glm::vec3& position, const glm::vec3& rotationEulerDegrees, const glm::vec3& scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, glm::radians(rotationEulerDegrees.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotationEulerDegrees.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotationEulerDegrees.z), glm::vec3(0, 0, 1));
    m = glm::scale(m, scale);
    return m;
}
