#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <glm/glm.hpp>

class FxSprites;
class World;

// Bullet impacts on the world (docs/BLOOD_FX.md, v2): what a round leaves where it strikes, by what it struck.
// The surface comes from the same words the impact sounds use (the collider's physics material, the tag, the
// name), with a finer table: concrete, brick, asphalt, rock, tile, metal, wood, glass, mud, sand.
//   - the hole: PRO Effects' textured decal for that surface (4 variants, turned at random) - on the static
//     world; a moving prop keeps the procedural hole that follows it (WeaponFxRenderer);
//   - the burst: PRO Effects' impact debris as flipbook sprites - chips of it (concrete, rock, brick, wood
//     splinters, glass shards), sparks off metal and stone. No dust or smoke (dropped: it read badly).
class ImpactFx {
public:
    void SetSprites(FxSprites* fx) { m_Sprites = fx; }
    bool Enabled = true;

    // "concrete=concrete,cement;metal=metal,steel;..." -> the first surface with a word in `name` (any case).
    static constexpr const char* kSurfaceTable =
        "metal=metal,steel,iron,grate,pipe;glass=glass,window;wood=wood,plank,crate,parquet,timber,plywood;"
        "brick=brick;asphalt=asphalt,road,tarmac;tile=tile,ceramic;sand=sand;mud=mud,dirt,soil,grass,gravel;"
        "rock=rock,stone,cliff,boulder;concrete=concrete,cement,wall,floor";
    static std::string SurfaceFromName(const std::string& name);
    std::string SurfaceOf(const World& world, std::uint32_t entity) const;
    // The surface's hole decal (a KnifeFxLibrary id; -1 when the library lacks it) and its size (metres across).
    static const char* HoleEntry(const std::string& surface);
    static float HoleSize(const std::string& surface, float radius);
    static float HoleFraction(const std::string& surface); // the hole's width in its decal, x the cell
    static float HoleRim(const std::string& surface);      // how far out its chipped rim shows (cell half-widths)
    // Whether `entity` is part of the static world (a decal projects onto it): no rigidbody, no skeleton.
    static bool IsStatic(const World& world, std::uint32_t entity);

    // The burst where a round travelling `dir` struck `surface` at `point` (surface normal `normal`).
    void Spawn(const std::string& surface, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir);
    int Spawned() const { return m_Spawned; }

private:
    FxSprites* m_Sprites = nullptr;
    int m_Spawned = 0;
    std::mt19937 m_Rng{0x1A2B3Cu};
    float U() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng); }
};
