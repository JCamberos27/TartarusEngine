#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Audio/WeaponAudio.h"

class World;
class AssetLibrary;
struct SprayPaintComponent;
namespace Graffiti {
struct Piece;
struct Program;
struct Timeline;
class Compositor;
} // namespace Graffiti

// The gameplay spray can (Decals P4, docs/GRAFFITI_DESIGNER.md "Spray can"): the player or an NPC sprays a
// stored .graffiti piece on to the wall in front of it. The piece goes on stroke by stroke at its recorded
// pace (Graffiti::Timeline), shown through its baked decal with a reveal order (DecalSystem::RevealOverride);
// the result is an ordinary entity (Decal + Spray Paint components) in the world, so it is saved with the game
// state, and a half-sprayed piece carries on after a load. The can is heard where it sprays: the rattle, the
// hiss held while paint is leaving it, the cap's release - 3D, through a SoundPlayer.
struct SprayCanAction {
    std::string Piece;                  // project-relative .graffiti (assets/Decals/Graffiti/Pieces/GP_*.graffiti)
    glm::vec3 From{0.0f};               // the can's ray: the eye (the player) or the hand / head (an NPC)
    glm::vec3 Direction{0.0f, 0.0f, 1.0f};
    float Reach = 2.5f;                 // metres: no wall within this, no spraying
    float Seconds = 0.0f;               // 0: the piece's own pace, held between kMinSeconds and kMaxSeconds
    float Scale = 1.0f;                 // x the piece's size on the wall
    std::string Painter = "Player";
    static constexpr float kMinSeconds = 4.0f, kMaxSeconds = 40.0f;
};

class SprayCanSystem {
public:
    SprayCanSystem();
    ~SprayCanSystem();

    // A wall hit: true with the point, its normal (toward the can) and what was hit.
    using RayFn = std::function<bool(const glm::vec3& from, const glm::vec3& dir, float reach, glm::vec3& point, glm::vec3& normal)>;
    void SetRaycast(RayFn fn) { m_Ray = std::move(fn); } // default: PhysicsWorld, triggers ignored

    // Casts for the wall and starts spraying: the new entity, or entt::null (no wall in reach, no such piece).
    // The piece is baked first if it never was (a moment's wait before it shows).
    entt::entity Start(World& world, const SprayCanAction& action);
    // Once per game frame: every Spray Paint in the world goes on (Elapsed += dt), its reveal and its sounds follow.
    void Update(World& world, float dt, const glm::vec3& listener);
    // Play's F6 (a dev key, no HUD): the player sprays the next piece of assets/Decals/Graffiti/Pieces at the
    // crosshair. Returns the entity started, or entt::null.
    entt::entity PlayerKeys(World& world, const glm::vec3& eye, const glm::vec3& front);
    // Play stopped: the sounds end, every reveal is dropped.
    void Stop();

    // 0..1 of the way through.
    static float Progress(const SprayPaintComponent& s);
    // The decal's pose on a wall: +Z out of it (along `normal`), the image's top up the wall (world up, or the
    // can's direction on a floor or ceiling).
    static glm::quat WallRotation(const glm::vec3& normal, const glm::vec3& canDirection);
    // The game state saved and read back (SceneSerializer, as a scene / the Play snapshot is) into a fresh world:
    // how many Spray Paint pieces come back with their Decal, and (optional) the sum of their Elapsed.
    static int RoundTrip(const World& world, AssetLibrary& assets, float* elapsedSum = nullptr);
    // The pieces the F6 key cycles through (project-relative, sorted).
    static std::vector<std::string> Pieces();

    // Every piece going on in `world`, for the tests (saving and reading the game state back when `assets` is given).
    struct Report {
        int Pieces = 0;
        float MinProgress = 1.0f, MaxProgress = 0.0f;
        bool Resident = false; // every piece's decal drawable now
        int Saved = -1;
        float SavedElapsed = 0.0f;
    };
    Report Inspect(World& world, AssetLibrary* assets);
    // State for the tests.
    int Spraying() const { return (int)m_Active.size(); }
    int SoundsPlayed() const { return m_SoundsPlayed; }
    bool Hissing() const;
    bool PieceReady(const std::string& piece); // baked and its reveal order made

private:
    struct PieceData {
        std::shared_ptr<Graffiti::Piece> Piece;
        std::shared_ptr<Graffiti::Program> Program;
        std::shared_ptr<Graffiti::Timeline> Timeline;
        std::string RevealKey;
        std::future<bool> Bake; // the first bake's PNG encode + write, on a worker
        bool Baked = false, BakeFailed = false;
    };
    struct Active {
        AudioEngine::SoundHandle Hiss = AudioEngine::InvalidHandle;
        bool Started = false, Finished = false;
    };
    PieceData* Data(const std::string& piece);
    void PlayAt(const char* file, const glm::vec3& at, float gain, bool loop, AudioEngine::SoundHandle* handle);

    RayFn m_Ray;
    std::unique_ptr<Graffiti::Compositor> m_Comp;
    std::unordered_map<std::string, std::unique_ptr<PieceData>> m_Pieces;
    std::unordered_map<entt::entity, Active> m_Active;
    SoundPlayer m_Sound;
    int m_SoundsPlayed = 0;
    int m_NextPiece = 0;
};
