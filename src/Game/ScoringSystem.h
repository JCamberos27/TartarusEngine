#pragma once

class World;
struct GameModuleHostAPI;

// Goal Trigger / Scoreboard / Score Digit (Components.h): counts goals from this frame's trigger
// events, updates the seven-segment digits, and plays each goal's burst, light flash and sound.
void UpdateScoring(const GameModuleHostAPI& host, World& world, float dt);

// Impact Sound (Components.h): plays each hit's sound from this frame's contact events.
void UpdateImpactSounds(const GameModuleHostAPI& host, World& world, float dt);
