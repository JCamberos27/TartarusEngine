#pragma once

class AssetLibrary;
class FirstPersonPresentation;
class NpcDirector;
class PlayerVitals;
class World;

// The Play-mode dev overlay (an ImGui window) for testing with the training dummies: god mode, infinite ammo,
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
    void Reset(PlayerVitals& vitals);

    struct Context {
        World* WorldPtr = nullptr;
        AssetLibrary* Assets = nullptr;
        NpcDirector* Npcs = nullptr;
        PlayerVitals* Vitals = nullptr;
        FirstPersonPresentation* Weapon = nullptr;
        bool* BloodLab = nullptr; // the Blood Lab window's open flag
    };
    void Draw(const Context& ctx); // call inside an ImGui frame while Open
};
