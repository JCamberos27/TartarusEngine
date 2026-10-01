#pragma once

class AssetLibrary;
class FirstPersonPresentation;
class NpcDirector;
class PlayerVitals;
class World;

// The Play-mode dev overlay (an ImGui window, F7) for testing the enemy squad: god mode, infinite ammo,
// freeze the AI, hold fire, invisible to the AI, difficulty, time scale, kill all / respawn the squad
// and the AI debug overlay. State only: main.cpp feeds the flags into the frame (ammo refill, the
// player's snapshot, the HUD's overlay switch).
//
// Hotkeys (Play, with an enemy squad): F7 the panel, F8 god mode, F9 the AI debug overlay.
class DevPanel {
public:
    bool Open = false;
    bool InfiniteAmmo = false;
    bool Invisible = false;      // the AI can't see or hear the player
    bool AiOverlay = false;      // lines and labels over the squad

    // A new Play: everything back to normal.
    void Reset(PlayerVitals& vitals, NpcDirector& npcs);

    struct Context {
        World* WorldPtr = nullptr;
        AssetLibrary* Assets = nullptr;
        NpcDirector* Npcs = nullptr;
        PlayerVitals* Vitals = nullptr;
        FirstPersonPresentation* Weapon = nullptr;
    };
    void Draw(const Context& ctx); // call inside an ImGui frame while Open

    // The panel's buttons, callable from tests.
    static int KillAll(World& world, NpcDirector& npcs);                    // returns how many were killed
    static int RespawnSquad(World& world, AssetLibrary& assets, NpcDirector& npcs); // fills the squad back up; returns how many spawned
};
