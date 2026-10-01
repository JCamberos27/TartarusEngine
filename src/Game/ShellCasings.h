#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

class AssetLibrary;
class Camera;
class Model;
class World;
struct MaterialAsset;

// One spent case thrown out of an ejection port (FirstPersonPresentation::TakeEjections).
struct CasingSpawn {
    std::string Model;    // the casing mesh
    std::string Material; // .mat for every submesh; empty keeps the import
    glm::vec3 Position{0.0f};
    glm::vec3 Velocity{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 AngularVelocity{0.0f}; // world space, rad/s
};

// The spent cases on the ground. Each is a model entity (lit and shadowed like any other) moved by a
// small integrator - gravity, tumble, a sphere sweep against the solid world, a few bounces - until it
// lies down on its side and sleeps, after which it costs nothing but its draw.
//
// Cases stay where they land. One is only ever removed while the camera can't see it (outside the
// view, or hidden behind something): once the player is DespawnDistance away from it, or - when more
// than SoftCap have piled up - oldest first. HardCap is a safety net for a pile that is all in view.
// Play-only state: the caller clears it when Play ends.
class ShellCasings {
public:
    struct Settings {
        float DespawnDistance = 25.0f; // metres from the player
        int SoftCap = 96;              // past this, the oldest unseen cases go
        int HardCap = 1024;            // past this, the oldest go even in view (never in practice)
        float Gravity = 9.81f;
        float Restitution = 0.3f;      // of the speed into a surface
        float Friction = 0.35f;        // of the speed along it, per bounce
        float ViewAspect = 2.4f;       // widest screen the view test allows for (wider = keeps more)
        int OcclusionRaysPerFrame = 8; // line-of-sight checks per frame; an unchecked case counts as seen
    };

    // One case for the lifetime rules (PickRemovals).
    struct LifeInput {
        glm::vec3 Position{0.0f};
        bool Seen = true;
        std::uint64_t Order = 0; // spawn order: lower is older
    };
    // Indices into `cases` to remove, by the rules above. Pure: no world, no physics, no GL.
    static std::vector<int> PickRemovals(const std::vector<LifeInput>& cases, const glm::vec3& player,
                                         const Settings& s);
    // Whether a sphere is inside the camera's view (generous: the far plane is ignored).
    static bool InView(const glm::vec3& camPos, const glm::vec3& front, const glm::vec3& right, const glm::vec3& up,
                       float fovDegrees, float aspect, const glm::vec3& point, float radius);

    void Spawn(World& world, AssetLibrary& assets, const CasingSpawn& spawn);
    // Moves the cases, then removes the ones the rules let go of.
    void Update(World& world, float dt, const Camera& camera, const glm::vec3& player);
    void Clear(World& world);

    Settings& Config() { return m_Settings; }
    int Count() const { return (int)m_Cases.size(); }
    int SleepingCount() const;
    std::uint64_t SpawnedTotal() const { return m_Order; }

private:
    struct Case {
        entt::entity Entity = entt::null;
        glm::vec3 Position{0.0f}, Velocity{0.0f}, AngularVelocity{0.0f};
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 LongAxis{0.0f, 1.0f, 0.0f}; // model space
        float Radius = 0.005f;                // across the case (the sweep)
        float Length = 0.04f;
        float GroundTime = 0.0f;              // time spent slow on the ground
        float Age = 0.0f;
        bool Sleeping = false;
        std::uint64_t Order = 0;
    };
    struct Kind {
        std::shared_ptr<Model> Mesh;
        std::shared_ptr<MaterialAsset> Material;
    };
    void Step(Case& c, float dt);

    Settings m_Settings;
    std::vector<Case> m_Cases;
    std::map<std::string, Kind> m_Kinds; // by model + material
    std::uint64_t m_Order = 0;
    std::mt19937 m_Rng{0x5eed};
};
