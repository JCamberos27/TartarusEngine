#include "SprayCan.h"

#include "Components.h"
#include "DecalSystem.h"
#include "EnginePaths.h"
#include "GameModuleAPI.h"
#include "GeneratedDecalMaps.h"
#include "GraffitiCompositor.h"
#include "GraffitiPaint.h"
#include "GraffitiPiece.h"
#include "GraffitiTimeline.h"
#include "Log.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "World.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
constexpr int kRevealSize = 512;      // the reveal order's longer side (texels)
constexpr float kRevealBand = 0.04f;  // of the piece's duration: how long a texel takes to come in
constexpr int kSpraySort = 50;        // over the authored wear, under the blood and holes
const char* const kStart = "assets/editor_sounds/SprayStart.wav";
const char* const kLoop = "assets/editor_sounds/SprayLoop.wav";
const char* const kEnd = "assets/editor_sounds/SprayEnd.wav";

std::string Abs(const std::string& p) {
    return p.empty() || fs::u8path(p).is_absolute() ? p : ProjectPaths::Resolve(p);
}
std::string Lower(std::string s) {
    for (char& c : s) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    return s;
}
// FNV-1a of the piece's recipe: its reveal order is made again only when the recipe changes.
uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
float SecondsFor(const SprayCanAction& a, float duration) {
    if (a.Seconds > 0.0f) return a.Seconds;
    return std::clamp(duration, SprayCanAction::kMinSeconds, SprayCanAction::kMaxSeconds);
}
} // namespace

SprayCanSystem::SprayCanSystem() {
    m_Ray = [](const glm::vec3& from, const glm::vec3& dir, float reach, glm::vec3& point, glm::vec3& normal) {
        const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit h;
        if (!PhysicsWorld::RaycastFiltered(o, d, reach, f, h) || !h.Hit || h.Entity == kPlayerEntity) return false;
        point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
        normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
        return true;
    };
}

SprayCanSystem::~SprayCanSystem() {
    Stop();
    for (auto& [k, p] : m_Pieces)
        if (p->Bake.valid()) p->Bake.wait();
}

float SprayCanSystem::Progress(const SprayPaintComponent& s) {
    return s.Seconds > 0.0f ? std::clamp(s.Elapsed / s.Seconds, 0.0f, 1.0f) : 1.0f;
}

glm::quat SprayCanSystem::WallRotation(const glm::vec3& normal, const glm::vec3& canDirection) {
    const glm::vec3 z = glm::normalize(normal);
    // Up the wall: world up flattened onto it; on a floor or a ceiling, away from the can.
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f) - z * z.y;
    if (glm::length(up) < 0.2f) up = canDirection - z * glm::dot(canDirection, z);
    if (glm::length(up) < 1e-4f) up = glm::vec3(1.0f, 0.0f, 0.0f) - z * z.x;
    const glm::vec3 y = glm::normalize(up), x = glm::normalize(glm::cross(y, z));
    return glm::quat_cast(glm::mat3(x, glm::cross(z, x), z));
}

int SprayCanSystem::RoundTrip(const World& world, AssetLibrary& assets, float* elapsedSum) {
    const std::string saved = SceneSerializer::SaveToString(world);
    World back;
    if (!SceneSerializer::LoadFromString(back, assets, saved)) return -1;
    int n = 0;
    float sum = 0.0f;
    for (auto [e, sp, dc] : back.Registry.view<const SprayPaintComponent, const DecalComponent>().each()) {
        (void)dc;
        ++n;
        sum += sp.Elapsed;
    }
    if (elapsedSum) *elapsedSum = sum;
    return n;
}

SprayCanSystem::Report SprayCanSystem::Inspect(World& world, AssetLibrary* assets) {
    Report r;
    DecalSystem* ds = DecalSystem::Current();
    r.Resident = ds != nullptr;
    for (auto [e, sp, dc] : world.Registry.view<const SprayPaintComponent, const DecalComponent>().each()) {
        ++r.Pieces;
        const float p = Progress(sp);
        r.MinProgress = std::min(r.MinProgress, p);
        r.MaxProgress = std::max(r.MaxProgress, p);
        r.Resident = r.Resident && ds->Resident(dc.AssetPath);
    }
    if (r.Pieces == 0) r.Resident = false;
    if (assets) r.Saved = RoundTrip(world, *assets, &r.SavedElapsed);
    return r;
}

std::vector<std::string> SprayCanSystem::Pieces() {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = fs::u8path(ProjectPaths::Resolve("assets/Decals/Graffiti/Pieces"));
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".graffiti") out.push_back("assets/Decals/Graffiti/Pieces/" + e.path().filename().u8string());
    std::sort(out.begin(), out.end());
    return out;
}

SprayCanSystem::PieceData* SprayCanSystem::Data(const std::string& piece) {
    const std::string key = Lower(Abs(piece));
    if (auto it = m_Pieces.find(key); it != m_Pieces.end()) return it->second.get();
    auto d = std::make_unique<PieceData>();
    d->Piece = Graffiti::Piece::Load(Abs(piece));
    if (!d->Piece || d->Piece->Missing) {
        Log::Warn("Spray can: no graffiti piece at " + piece);
        m_Pieces.emplace(key, nullptr);
        return nullptr;
    }
    if (!m_Comp) m_Comp = std::make_unique<Graffiti::Compositor>();
    d->Program = std::make_shared<Graffiti::Program>(Graffiti::Build(*d->Piece, m_Comp->MetricsCallback()));
    d->Timeline = std::make_shared<Graffiti::Timeline>(Graffiti::BuildTimeline(*d->Program));
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)Fnv(d->Piece->ToString()));
    d->RevealKey = "spray_" + d->Piece->Name + "_" + hex;
    // Baked already (the Designer, or an earlier spray)? Else bake it now: the GPU part here, the PNGs on a worker.
    std::error_code ec;
    d->Baked = fs::exists(fs::u8path(d->Piece->DecalPath()), ec) && fs::exists(fs::u8path(d->Piece->MapPath("Albedo")), ec);
    if (!d->Baked) {
        Graffiti::BakeImages img;
        m_Comp->StartBake(*d->Piece, *d->Program);
        if (m_Comp->PollBake(img, /*wait=*/true)) {
            auto recipe = d->Piece;
            d->Bake = std::async(std::launch::async, [recipe, img]() { return Graffiti::WriteBake(*recipe, Graffiti::EncodePngs(img)); });
        } else {
            d->BakeFailed = true;
        }
    }
    PieceData* raw = d.get();
    m_Pieces.emplace(key, std::move(d));
    return raw;
}

bool SprayCanSystem::PieceReady(const std::string& piece) {
    PieceData* d = Data(piece);
    if (!d || !d->Baked) return false;
    return GeneratedDecalMaps::Get().Find(d->RevealKey, 512, nullptr) != nullptr;
}

void SprayCanSystem::PlayAt(const char* file, const glm::vec3& at, float gain, bool loop, AudioEngine::SoundHandle* handle) {
    SoundSet set;
    set.Key = std::string("spraycan.") + file;
    set.Files = {EnginePaths::Resolve(file)};
    set.FilesExplicit = true;
    set.Volume = gain;
    set.Loop = loop;
    set.MinDistance = 1.0f;
    set.MaxDistance = 18.0f;
    set.PitchMin = loop ? 1.0f : 0.95f;
    set.PitchMax = loop ? 1.0f : 1.05f;
    set.MaxVoices = 6;
    SoundPlayer::Request r;
    r.Position = at;
    r.At2D = false;
    const SoundPlayer::Played p = m_Sound.Play(set, r);
    ++m_SoundsPlayed;
    if (handle) *handle = p.Started ? p.Handle : AudioEngine::InvalidHandle;
}

entt::entity SprayCanSystem::Start(World& world, const SprayCanAction& a) {
    PieceData* d = Data(a.Piece);
    if (!d || d->BakeFailed) return entt::null;
    const float len = glm::length(a.Direction);
    if (len < 1e-5f) return entt::null;
    const glm::vec3 dir = a.Direction / len;
    glm::vec3 point, normal;
    if (!m_Ray || !m_Ray(a.From, dir, a.Reach, point, normal)) return entt::null;
    if (glm::dot(normal, dir) > 0.0f) normal = -normal;

    const std::string rel = ProjectPaths::Relativize(Abs(a.Piece));
    const entt::entity e = world.CreateEmptyEntity(point + normal * 0.01f, glm::vec3(0.0f), glm::vec3(1.0f),
                                                   "Graffiti (" + d->Piece->Name + ")");
    if (auto* t = world.Registry.try_get<TransformComponent>(e)) t->Rotation = WallRotation(normal, dir);
    DecalComponent dc;
    dc.AssetPath = Abs(d->Piece->DecalPath());
    dc.Size = glm::vec3(d->Piece->Metres * std::max(a.Scale, 0.05f), 0.25f);
    dc.SortOrder = kSpraySort;
    world.Registry.emplace<DecalComponent>(e, dc);
    SprayPaintComponent sp;
    sp.PiecePath = rel;
    sp.Seconds = SecondsFor(a, d->Timeline->Duration);
    sp.Elapsed = 0.0f;
    sp.Painter = a.Painter;
    world.Registry.emplace<SprayPaintComponent>(e, sp);
    std::printf("[SprayCan] %s sprays %s on a wall at (%.2f, %.2f, %.2f) over %.1f s\n", a.Painter.c_str(), rel.c_str(), point.x,
                point.y, point.z, sp.Seconds);
    return e;
}

bool SprayCanSystem::Hissing() const {
    for (const auto& [e, a] : m_Active)
        if (a.Hiss != AudioEngine::InvalidHandle) return true;
    return false;
}

void SprayCanSystem::Update(World& world, float dt, const glm::vec3& listener) {
    m_Sound.SetListener(listener);
    m_Sound.Update(dt);
    DecalSystem* ds = DecalSystem::Current();
    // First bakes finishing: the decal re-read with its new maps.
    for (auto& [key, d] : m_Pieces) {
        if (!d || !d->Bake.valid() || d->Bake.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        d->Baked = d->Bake.get();
        d->BakeFailed = !d->Baked;
        if (d->Baked && ds) ds->Refresh(d->Piece->DecalPath());
    }
    auto& reg = world.Registry;
    for (auto [e, sp, dc] : reg.view<SprayPaintComponent, const DecalComponent>().each()) {
        PieceData* d = Data(sp.PiecePath);
        Active& act = m_Active[e];
        const float before = Progress(sp);
        if (before >= 1.0f && !act.Started) { // finished before this session (a loaded game): just the piece
            if (ds) ds->ClearReveal((uint32_t)entt::to_integral(e));
            act.Finished = true;
            continue;
        }
        if (act.Finished) continue;
        if (!d || !d->Baked) {
            // Not baked yet: nothing goes on, nothing shows (an empty reveal hides the decal until it's ready).
            continue;
        }
        // The reveal order, made once per recipe on a worker.
        const Graffiti::Program& prog = *d->Program;
        const Graffiti::Timeline& tl = *d->Timeline;
        const int rw = prog.Width >= prog.Height ? kRevealSize : std::max(1, kRevealSize * prog.Width / std::max(prog.Height, 1));
        const int rh = prog.Width >= prog.Height ? std::max(1, kRevealSize * prog.Height / std::max(prog.Width, 1)) : kRevealSize;
        const std::shared_ptr<const Graffiti::Program> progPtr = d->Program; // the job may outlive this system
        const std::shared_ptr<const Graffiti::Timeline> tlPtr = d->Timeline;
        const DecalTextureArrays::Source* reveal = GeneratedDecalMaps::Get().Find(d->RevealKey, 512, [progPtr, tlPtr, rw, rh]() -> GeneratedDecalMaps::Job {
            return [progPtr, tlPtr, rw, rh]() {
                GeneratedDecalMaps::Images im;
                im.Width = rw;
                im.Height = rh;
                im.Aux = Graffiti::RevealMap(*progPtr, *tlPtr, rw, rh);
                return im;
            };
        });
        const glm::vec3 at = glm::vec3(world.GetCachedWorldTransform(e)[3]);
        if (!reveal) { // still being made: hold the piece hidden (a threshold over every texel)
            if (ds) ds->SetReveal((uint32_t)entt::to_integral(e), {"", 1.0f, kRevealBand});
            continue;
        }
        sp.Elapsed = std::min(sp.Elapsed + dt, sp.Seconds);
        const float p = Progress(sp);
        if (ds) {
            if (p < 1.0f) ds->SetReveal((uint32_t)entt::to_integral(e), {reveal->Aux, Graffiti::RevealThreshold(p, kRevealBand), kRevealBand});
            else ds->ClearReveal((uint32_t)entt::to_integral(e));
        }
        // Where the can is now, on the wall: the sound comes from there.
        glm::vec2 uv(0.5f);
        const bool spraying = p < 1.0f && tl.SprayingAt(prog, p * tl.Duration, &uv);
        const glm::mat4 m = world.GetCachedWorldTransform(e);
        const glm::vec3 can = glm::vec3(m * glm::vec4((uv.x - 0.5f) * dc.Size.x, (0.5f - uv.y) * dc.Size.y, 0.15f, 1.0f));
        if (!act.Started) {
            act.Started = true;
            PlayAt(kStart, at, 0.8f, false, nullptr);
        }
        if (spraying && act.Hiss == AudioEngine::InvalidHandle) PlayAt(kLoop, can, 0.55f, true, &act.Hiss);
        if (!spraying && act.Hiss != AudioEngine::InvalidHandle) {
            m_Sound.Backend().Stop(act.Hiss);
            act.Hiss = AudioEngine::InvalidHandle;
        }
        if (act.Hiss != AudioEngine::InvalidHandle) m_Sound.Backend().SetVoicePosition(act.Hiss, can);
        if (p >= 1.0f) {
            act.Finished = true;
            PlayAt(kEnd, can, 0.8f, false, nullptr);
            std::printf("[SprayCan] %s finished %s\n", sp.Painter.c_str(), sp.PiecePath.c_str());
        }
    }
    // Entities gone (Play stopped, destroyed): their sounds end.
    for (auto it = m_Active.begin(); it != m_Active.end();) {
        if (reg.valid(it->first) && reg.all_of<SprayPaintComponent>(it->first)) { ++it; continue; }
        if (it->second.Hiss != AudioEngine::InvalidHandle) m_Sound.Backend().Stop(it->second.Hiss);
        if (ds) ds->ClearReveal((uint32_t)entt::to_integral(it->first));
        it = m_Active.erase(it);
    }
}

entt::entity SprayCanSystem::PlayerKeys(World& world, const glm::vec3& eye, const glm::vec3& front) {
    static bool held = false;
    GLFWwindow* w = glfwGetCurrentContext();
    const bool down = w && glfwGetKey(w, GLFW_KEY_F6) == GLFW_PRESS;
    const bool pressed = down && !held;
    held = down;
    if (!pressed) return entt::null;
    const std::vector<std::string> pieces = Pieces();
    if (pieces.empty()) {
        Log::Warn("Spray can (F6): no graffiti pieces in assets/Decals/Graffiti/Pieces");
        return entt::null;
    }
    SprayCanAction a;
    a.Piece = pieces[(size_t)m_NextPiece++ % pieces.size()];
    a.From = eye;
    a.Direction = front;
    a.Painter = "Player";
    const entt::entity e = Start(world, a);
    if (e == entt::null) Log::Info("Spray can (F6): no wall within reach");
    return e;
}

void SprayCanSystem::Stop() {
    DecalSystem* ds = DecalSystem::Current();
    for (auto& [e, a] : m_Active) {
        if (a.Hiss != AudioEngine::InvalidHandle) m_Sound.Backend().Stop(a.Hiss);
        if (ds) ds->ClearReveal((uint32_t)entt::to_integral(e));
    }
    m_Active.clear();
}
