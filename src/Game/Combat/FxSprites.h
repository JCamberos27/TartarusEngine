#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

class FxSpriteRenderer;

// Flipbook particles (docs/BLOOD_FX.md, "v2"): the short-lived billboards of the Knife packs - blood
// mist and bursts on a hit, impact dust and debris, muzzle flashes and smoke. A pooled CPU sim (motion
// under gravity and drag, a bounce off one plane, size / alpha / erosion over life, the flipbook's frame);
// Submit() hands the live ones to the FxSpriteRenderer, which draws them all in one sorted instanced draw.
// Owns no GL. Textures are KnifeFxLibrary entries, looked up by name once.
class FxSprites {
public:
    enum class Shade : std::uint8_t {
        Blood,    // the entry's white shape eroded over life, tinted, glossy and lit (normal-mapped)
        Lit,      // lit soft colour: the entry's albedo (debris) or white density (smoke, dust) x tint
        Additive, // emission: shape x colour, added (muzzle flashes, sparks, glows)
    };
    struct Emit {
        int Entry = -1;              // KnifeFxLibrary id; -1 = a procedural soft dot (sparks)
        Shade Mode = Shade::Lit;
        glm::vec3 Pos{0.0f}, Vel{0.0f};
        float Gravity = 0.0f;        // x 9.81 m/s^2 down
        float Drag = 0.0f;           // 1/s
        float Life = 1.0f;
        float Size0 = 0.1f, Size1 = 0.1f; // height in metres, over life (smoothstep-free: linear)
        float Rot = 0.0f, Spin = 0.0f;    // radians, radians / s (camera-facing only)
        int Frame = 0;               // first cell; -1 = a random one (variants: debris, holes)
        bool Animate = true;         // run the flipbook over the life (else hold Frame)
        bool BlendFrames = false;    // cross-fade between cells (smoke)
        glm::vec3 Color{1.0f};       // linear; x Intensity for additive
        float Intensity = 1.0f;
        float Alpha = 1.0f;
        float FadeIn = 0.0f, FadeOut = 0.3f; // fractions of the life
        float Erosion0 = 0.0f, Erosion1 = 0.0f, Softness = 0.25f; // Blood: the shape's threshold over life
        float Stretch = 0.0f;        // > 0: drawn along its velocity, longer by speed x Stretch
        glm::vec3 Axis{0.0f};        // non-zero: drawn along this world axis instead (muzzle side flash)
        glm::vec4 Plane{0.0f};       // xyz normal, w offset: bounces off it (n.p + w = 0); zero = none
        float Bounce = 0.3f;
        bool ViewModel = false;      // drawn with the arms and gun (the player's own muzzle)
        int Follow = 0;              // non-zero: Follow(tag, ...) keeps it on a moving emitter (a muzzle)
        float FollowAhead = 0.0f;    // ... this far along the emitter's axis
    };

    // Entry name -> id (KnifeFxLibrary by default; tests replace it). Frames / cols come with it.
    struct EntryInfo { int Frames = 1; float Aspect = 1.0f; };
    using LookupFn = std::function<int(const std::string& name, EntryInfo& info)>;
    void SetLookup(LookupFn fn) { m_Lookup = std::move(fn); }
    int Entry(const std::string& name); // cached; -1 when the library lacks it

    int MaxLive = 1500; // the oldest go first past this
    void Spawn(const Emit& e);
    void Update(float dt);
    // Every live sprite spawned with Follow == `tag` back onto the emitter at `pos`, along `axis` (unit).
    void Follow(int tag, const glm::vec3& pos, const glm::vec3& axis);
    void Clear() { m_Live.clear(); }
    void Submit(FxSpriteRenderer& r) const;

    struct Particle {
        Emit E;
        float Age = 0.0f;
        int Frames = 1;
        float Aspect = 1.0f;
        std::uint32_t Seed = 0;
    };
    const std::vector<Particle>& Live() const { return m_Live; }
    int Spawned() const { return m_Spawned; }

    // The flipbook cell and cross-fade at `t` (0..1 of the life) for `frames` cells starting at `first`.
    static void FrameAt(float t, int frames, int first, bool animate, int& cellA, int& cellB, float& blend);

private:
    std::vector<Particle> m_Live;
    LookupFn m_Lookup;
    struct Cached { std::string Name; int Id; EntryInfo Info; };
    std::vector<Cached> m_Cache;
    int m_Spawned = 0;
    std::uint32_t m_Rng = 0x9E3779B9u;
    float Rand();
};
