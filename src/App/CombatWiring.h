#pragma once
#include <cstdint>

#include <glm/glm.hpp>

class BloodFx;
class BloodLab;
class BulletHoleList;
class CombatFx;
class FirstPersonPresentation;
class FxSprites;
class ImpactFx;
class NpcDirector;
class NpcTest;
class Player;
class SpiderDamage;
class SprayCanSystem;
class World;

// What the combat wiring hooks together (the Renderer/ShadowFrame pattern: pointers to main's long-lived objects, no
// ownership). Every object here must outlive the CombatWiring that wired it.
struct CombatWiringInputs {
    World* world = nullptr;
    Player* player = nullptr;
    FirstPersonPresentation* firstPersonPresentation = nullptr;
    NpcDirector* npcDirector = nullptr;
    SpiderDamage* spiderDamage = nullptr;
    CombatFx* combatFx = nullptr;
    BloodFx* bloodFx = nullptr;
    ImpactFx* impactFx = nullptr;
    FxSprites* fxSprites = nullptr;
    BulletHoleList* bulletHoles = nullptr;
    SprayCanSystem* sprayCans = nullptr;
    BloodLab* bloodLab = nullptr;
    NpcTest* npcTest = nullptr; // --npc-test, else null
};

// Play's combat hooked together once at start-up: the FX sharing the flipbook sprites, the blood's lookups into the
// soldiers, spiders, props and the player's own body and gun, the footstep listener behind bloody footprints, the
// Blood Lab's crosshair actions, and the --npc-test blood / decal / spray hooks.
class CombatWiring {
public:
    CombatWiring() = default;
    CombatWiring(const CombatWiring&) = delete;
    CombatWiring& operator=(const CombatWiring&) = delete;
    // The footstep listener captures the blood and the player by reference: dropped here, before they are destroyed
    // (declare this after them).
    ~CombatWiring();

    void Wire(const CombatWiringInputs& in);

    // A round into the world: the surface's burst, and its textured hole on the static world (the procedural
    // one follows a moving prop).
    void WorldImpact(std::uint32_t entity, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir,
                     float radius) const;

private:
    CombatWiringInputs m_In;
    bool m_StepListener = false;
};
