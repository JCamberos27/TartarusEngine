#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class AssetLibrary;
class FxSprites;
class Model;
class World;

// Headshot gore (docs/BLOOD_FX.md, v2): a round with the energy to take a head apart does. The soldier's head
// (his body from the head bone up; his clothing from the neck up: the balaclava, the hood) stops drawing
// (GoreHideTag); Real Blood's burst stump stands on his neck in its place and follows it as he falls;
// a few brain chunks are thrown out along the round's line, tumble, bounce and lie; the air fills with the
// burst. Only on the big ones - Blood Settings' Gore at Full (2), a head kill, energy past kBurstEnergy.
// The meshes are the Knife packs' (tools/knife_gore_bake.py -> assets/Effects/Knife/Gore); without them none of
// this happens. Play-only state: Clear when Play ends.
class HeadGore {
public:
    static constexpr float kBurstEnergy = 1.3f;
    static bool ShouldBurst(bool head, bool killed, bool player, float energy, int goreLevel) {
        return head && killed && !player && goreLevel >= 2 && energy >= kBurstEnergy;
    }

    // A body's drawing pieces (driver + clothing) and where its neck and head bones are now (world).
    struct Body {
        std::vector<entt::entity> Pieces;
        glm::vec3 Neck{0.0f}, Head{0.0f};
        glm::vec3 Forward{0.0f, 0.0f, 1.0f}; // which way the body faces
    };
    using BodyFn = std::function<bool(unsigned entity, Body& out)>;
    void SetBody(BodyFn fn) { m_Body = std::move(fn); }
    void SetSprites(FxSprites* fx) { m_Sprites = fx; }

    // The head at `point` blown apart by a round travelling `dir`. False when nothing happened (no body, no meshes,
    // already done).
    bool Burst(World& world, AssetLibrary& assets, unsigned entity, const glm::vec3& point, const glm::vec3& dir, float energy);
    void Update(World& world, float dt);
    void Clear(World& world);

    int Bursts() const { return m_Bursts; }
    int Chunks() const { return (int)m_Chunks.size(); }
    bool Headless(unsigned entity) const;

    // The bones at and under `root` as a palette bit mask (GoreHideTag); false when the model lacks it.
    static bool MaskUnder(const Model& m, const char* root, std::uint32_t (&bits)[16]);
    // The stump's pose on a neck: base at `head`, up along neck -> head, its front toward `forward`.
    static glm::quat StumpRotation(const glm::vec3& neck, const glm::vec3& head, const glm::vec3& forward);

private:
    struct Victim { unsigned Entity; entt::entity Stump = entt::null; };
    struct Chunk {
        entt::entity Entity = entt::null;
        glm::vec3 Position{0.0f}, Velocity{0.0f}, Spin{0.0f};
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float Radius = 0.03f, Age = 0.0f;
        bool Sleeping = false;
    };
    bool LoadMeshes(AssetLibrary& assets);
    void Step(Chunk& c, float dt);
    BodyFn m_Body;
    FxSprites* m_Sprites = nullptr;
    std::shared_ptr<Model> m_Stump;
    std::vector<std::shared_ptr<Model>> m_Brain;
    bool m_LoadTried = false;
    std::vector<Victim> m_Victims;
    std::vector<Chunk> m_Chunks;
    int m_Bursts = 0;
    std::mt19937 m_Rng{0x60E5u};
};
