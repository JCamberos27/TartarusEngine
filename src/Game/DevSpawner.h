#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <functional>
#include <vector>

class World;
class AssetLibrary;
class NpcDirector;
class FirstPersonPresentation;

// F9-F12 in Play (FKeyRow.h): F9 a training dummy, F11 an item, each at the crosshair and facing the player; Shift
// picks the next variant without spawning. F12 takes away everything they made, corpses included. The items are real
// things on the floor: a gun or an ammo box the player picks up with Interact (E), or a physics crate.
class DevSpawner {
public:
    struct Context {
        World* WorldPtr = nullptr;
        AssetLibrary* Assets = nullptr;
        NpcDirector* Npcs = nullptr;
        FirstPersonPresentation* Gun = nullptr; // the player's weapons: what a pickup gives
        glm::vec3 Eye{0.0f}, Front{0.0f, 0.0f, -1.0f};
        float Dt = 0.0f;
        bool Keys = false;     // the F-row may act (Play, the game has input, not typing)
        bool Interact = false; // Interact pressed this frame
        // Starts the dummies in a scene with no Dummy Spawn (NpcDirector::Start allowEmpty); false when it can't.
        std::function<bool()> StartSquad;
    };
    void Update(const Context& c);
    void Stop(World& world); // the props and guns go now (F12 uses it)
    // Play ended: the play world took everything with it, so just let go of the handles (never destroy by id in the
    // edit world: the ids are reused).
    void Forget();

    int Spawned() const; // what F12 would clear (tests)

private:
    void SpawnNpc(const Context& c);
    void SpawnItem(const Context& c);
    void Clear(const Context& c);
    void Pickups(const Context& c);
    void Place(const Context& c, glm::vec3& at, float& yawDeg) const;
    bool m_Warmed = false; // this Play's dummy body prefab has been warmed
    int m_Npc = 0, m_Item = 0;
    bool m_WaitSquad = false; // started the dummies this press: spawn once their navmesh is up
    std::vector<int> m_NpcIds;
    std::vector<entt::entity> m_Props;
    std::vector<entt::entity> m_Pickups; // ammo boxes, picked up with Interact
};
