#pragma once

class AssetLibrary;
class FirstPersonPresentation;
class NpcDirector;
class PlayerVitals;
class World;

// The Play-mode dev overlay (an ImGui window, F7) for testing with the training dummies: god mode, infinite ammo,
// time scale, kill all / respawn the dummies, and the AI debug overlay. State only: the host feeds the flags into the frame
// (ammo refill, the HUD's overlay switch).
//
// Hotkeys (Play, with dummies; no default keys): the panel, god mode, the AI debug overlay.
class DevPanel {
public:
    bool Open = false;
    bool InfiniteAmmo = false;
    bool AiOverlay = false;      // each dummy's goal and a label over it

    // A new Play: everything back to normal.
    void Reset(PlayerVitals& vitals, NpcDirector& npcs);
    // F7 the panel, F8 god mode: call once per Play frame.

    struct Context {
        World* WorldPtr = nullptr;
        AssetLibrary* Assets = nullptr;
        NpcDirector* Npcs = nullptr;
        PlayerVitals* Vitals = nullptr;
        FirstPersonPresentation* Weapon = nullptr;
        bool* BloodLab = nullptr; // the Blood Lab window's open flag
    };
    void Draw(const Context& ctx); // call inside an ImGui frame while Open

    // The panel's buttons, callable from tests.
    static int KillAll(World& world, NpcDirector& npcs);                    // returns how many were killed
    static int RespawnSquad(World& world, AssetLibrary& assets, NpcDirector& npcs); // a dummy at every spawn that has none; returns how many spawned
};
