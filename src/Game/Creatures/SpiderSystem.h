#pragma once

#include "IK.h"
#include "SpiderLocomotion.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class AssetLibrary;
class World;

// The enemy spiders in Play: every active entity with a Spider component (a spider prefab's root, its model and Animator
// Controller on a child). Think moves each body over the surfaces toward its goal before the animators run; LatePose,
// after them, steps the legs and puts them onto their footholds with IK (SpiderLocomotion.h has the how).
class SpiderSystem {
public:
    void Think(World& world, AssetLibrary& assets, float dt);
    void LatePose(World& world, float dt);
    void Clear() { m_Bodies.clear(); }
    bool Active() const { return !m_Bodies.empty(); }

    // A spider from `prefab` (a scene file whose root carries a Spider component) at `pos` facing `rot`: its root, or
    // entt::null when the prefab won't load.
    entt::entity Spawn(World& world, AssetLibrary& assets, const std::string& prefab, const glm::vec3& pos, const glm::quat& rot);

    // What a test reads back about one spider (by its root).
    struct Stats {
        glm::vec3 Pos{0.0f}, Up{0.0f, 1.0f, 0.0f}, Forward{0.0f, 0.0f, -1.0f};
        glm::vec3 SurfaceNormal{0.0f, 1.0f, 0.0f}, LegNormal{0.0f}; // under the body; the planted legs' mean
        float Speed = 0.0f, SurfaceGap = 0.0f;
        bool Ready = false, Attached = false, Arrived = false;
        int Lifted[2] = {0, 0};      // legs of each gait group mid-step now
        int BothLifted = 0;          // frames ever with both groups lifted at once
        int Steps = 0;               // steps finished, all legs
        int Grounded = 0;            // legs standing on something now
        float TipError = 0.0f;       // the worst leg's IK miss this frame (m): tip vs where the gait put the foot
        float PlantedSlide = 0.0f;   // the worst planted foot's travel this frame (m): should be none
        int Rays = 0;                // surface rays this frame
    };
    bool Inspect(entt::entity root, Stats& out) const;

private:
    struct Body {
        entt::entity Root = entt::null, ModelEntity = entt::null;
        bool Ready = false, GaitStarted = false;
        Spider::Mover Mover;
        Spider::Gait Gait;
        std::array<std::vector<int>, Spider::kLegs> Chains{};
        std::array<glm::vec3, Spider::kLegs> TipOffset{};
        int Thorax = -1;
        std::vector<int> Parents;
        IK::Pose Pose;
        std::vector<glm::mat4> Globals;
        std::array<glm::vec3, Spider::kLegs> PrevFoot{};
        std::array<bool, Spider::kLegs> PrevPlanted{};
        Stats Last;
    };
    bool Resolve(World& world, AssetLibrary& assets, Body& b);

    std::vector<Body> m_Bodies;
    std::uint64_t m_Generation = 0;
    std::string m_RigPath, m_RigText;
    Spider::RigDef m_Rig;
    bool m_RigOk = false;
    std::string m_PrefabPath, m_PrefabText;
};
