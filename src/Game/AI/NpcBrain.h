#pragma once

#include <glm/glm.hpp>

class NpcDirector;
class World;
struct Npc;
struct PlayerSnapshot;

// What a soldier decides to do. Utility selection between behaviours (Npc.h's Behaviour): each is
// scored from what the soldier knows - health, ammo, cover, its squad role and tokens, how long
// since it saw the player, whether it's under fire - several times a second, with some inertia so
// it commits; the winner runs as a small state machine that writes the frame's NpcIntent (where to
// go and how fast, what to aim at, whether to crouch, lean, shoot, reload).
class NpcBrain {
public:
    static void Think(NpcDirector& d, World& world, Npc& n, const PlayerSnapshot& p, float dt);

    // Cover search goals.
    enum class CoverGoal { Fight, Flank, Push, Retreat };
    // The best cover point for `goal` against a threat at `threat` (feet), claimed for `n`; -1 for none.
    static int FindCover(NpcDirector& d, Npc& n, CoverGoal goal, const glm::vec3& threat);

private:
    static int Execute(NpcDirector& d,Npc& n,const PlayerSnapshot& p,int operation,int target,const glm::vec3* threat=nullptr);
    static void Choose(NpcDirector& d, Npc& n, const PlayerSnapshot& p);
    static void Enter(NpcDirector& d, Npc& n, int behaviour, const PlayerSnapshot& p);
    static void Run(NpcDirector& d, World& world, Npc& n, const PlayerSnapshot& p, float dt);
};
