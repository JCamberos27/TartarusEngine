#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <vector>

class World;
class AssetLibrary;
class NpcDirector;
class SpiderSystem;
class SpiderDamage;
class FirstPersonPresentation;
class NpcDroppedWeapon;

// F9-F12 in Play (FKeyRow.h): F9 a soldier, F10 a spider, F11 an item, each at the crosshair and facing the player; Shift
// picks the next variant without spawning. F12 takes away everything they made, corpses included. The items are real
// things on the floor: a gun or an ammo box the player picks up with Interact (E), or a physics crate.
class DevSpawner {
public:
    struct Context {
        World* WorldPtr = nullptr;
        AssetLibrary* Assets = nullptr;
        NpcDirector* Npcs = nullptr;
        SpiderSystem* Spiders = nullptr;
        SpiderDamage* SpiderHurt = nullptr; // F12 takes a spider's hitboxes and ragdoll down before its entity
        FirstPersonPresentation* Gun = nullptr; // the player's weapons: what a pickup gives
        glm::vec3 Eye{0.0f}, Front{0.0f, 0.0f, -1.0f};
        float Dt = 0.0f;
        bool Keys = false;     // the F-row may act (Play, the game has input, not typing)
        bool Interact = false; // Interact pressed this frame
        // Starts the soldiers in a scene with no NPC Spawn (NpcDirector::Start allowEmpty); false when it can't.
        std::function<bool()> StartSquad;
    };
    DevSpawner();
    ~DevSpawner(); // the pickups' guns are complete types only in DevSpawner.cpp
    void Update(const Context& c);
    void Stop(World& world); // the props and guns go now (F12 uses it)
    // Play ended: the play world took everything with it, so just let go of the handles (never destroy by id in the
    // edit world: the ids are reused).
    void Forget();

    int Spawned() const; // what F12 would clear (tests)

private:
    struct Pickup {
        int Kind = 0; // 0 AK, 1 Shotgun, 2 Ammo
        std::unique_ptr<NpcDroppedWeapon> Gun;
        entt::entity Prop = entt::null; // the ammo box
    };
    void SpawnNpc(const Context& c);
    void SpawnSpider(const Context& c);
    void SpawnItem(const Context& c);
    void Clear(const Context& c);
    void Pickups(const Context& c);
    bool Place(const Context& c, glm::vec3& at, float& yawDeg) const;
    void Warm(const Context& c);
    bool m_Warmed = false; // this Play's spawn assets asked for (Warm)
    int m_Npc = 0, m_Spider = 0, m_Item = 0;
    bool m_WaitSquad = false; // started the soldiers this press: spawn once their navmesh is up
    std::vector<int> m_NpcIds;
    std::vector<entt::entity> m_SpiderIds, m_Props;
    std::vector<Pickup> m_Pickups;
};
