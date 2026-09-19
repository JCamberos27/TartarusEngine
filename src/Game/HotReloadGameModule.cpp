#include "HotReloadGameModule.h"

#include "GameModuleAPI.h"
#include "Log.h"
#include "World.h"
#include "PhysicsWorld.h" // #185 PR 2 — Raycast is backed by the host-side PhysX world
#include "HotReloadSwap.h"
#include "TimeService.h"
#include "AudioEngine.h"
#include "ProjectPaths.h"
#include "Input.h"
#include "InputMap.h"
#include <algorithm>
#include <filesystem>
#include <unordered_set>

#include <string>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

// The host callback table handed to the game module every Update. The physics callbacks are
// thin forwards to the PhysX world and all no-op (miss / 0 events) whenever it isn't live, so
// a module calling them outside Play just gets nothing back. #185 PR 2 added Raycast, PR 5
// GetTriggerEvents.
void GetTimeFn(GameTime& out) {
    out.DeltaTime = Time::DeltaTime();
    out.UnscaledDeltaTime = Time::UnscaledDeltaTime();
    out.FixedDeltaTime = Time::FixedDeltaTime();
    out.TimeScale = Time::EffectiveTimeScale();
    out.Time = Time::TimeSinceStartup();
    out.UnscaledTime = Time::UnscaledTimeSinceStartup();
    out.RealtimeSinceStartup = Time::RealtimeSinceStartup();
    out.FrameCount = Time::FrameCount();
}

GameModuleHostAPI MakeHostAPI();
const GameModuleHostAPI kHostAPI = MakeHostAPI();

GameModuleHostAPI MakeHostAPI() {
    // Assigned by name, like the editor host's table: same-signature slots can't be swapped.
    GameModuleHostAPI api;
    api.Version = kGameModuleAPIVersion;
    api.GetTime = &GetTimeFn;
    api.SetTimeScale = [](float scale) { Time::SetTimeScale(scale); };
    const GameModuleHostAPI physics{
    kGameModuleAPIVersion,
    /*Raycast=*/[](const float origin[3], const float dir[3], float maxDistance, RaycastHit& outHit) {
        return PhysicsWorld::Raycast(origin, dir, maxDistance, outHit);
    },
    /*GetTriggerEvents=*/[](TriggerEvent* out, int maxEvents) {
        return PhysicsWorld::GetTriggerEvents(out, maxEvents);
    },
    /*AddForce=*/[](std::uint32_t e, const float f[3], std::uint32_t m) { PhysicsWorld::AddForce(e, f, m); },
    /*AddTorque=*/[](std::uint32_t e, const float tq[3], std::uint32_t m) { PhysicsWorld::AddTorque(e, tq, m); },
    /*AddForceAtPosition=*/[](std::uint32_t e, const float f[3], const float p[3], std::uint32_t m) {
        PhysicsWorld::AddForceAtPosition(e, f, p, m);
    },
    /*AddExplosionForce=*/[](const float c[3], float r, float s, float up) {
        PhysicsWorld::AddExplosionForce(c, r, s, up);
    },
    /*SetLinearVelocity=*/[](std::uint32_t e, const float v[3]) { PhysicsWorld::SetLinearVelocity(e, v); },
    /*GetBodyState=*/[](std::uint32_t e, BodyState& out) { return PhysicsWorld::GetBodyState(e, out); },
    /*GetContactEvents=*/[](ContactEvent* out, int maxEvents) {
        return PhysicsWorld::GetContactEvents(out, maxEvents);
    },
    /*SphereCast=*/[](const float o[3], const float d[3], float r, float maxDist, RaycastHit& hit) {
        return PhysicsWorld::SphereCast(o, d, r, maxDist, hit);
    },
    /*OverlapSphere=*/[](const float c[3], float r, std::uint32_t* out, int maxE) {
        return PhysicsWorld::OverlapSphere(c, r, out, maxE);
    },
    };
    api.Raycast = physics.Raycast;
    api.GetTriggerEvents = physics.GetTriggerEvents;
    api.AddForce = physics.AddForce;
    api.AddTorque = physics.AddTorque;
    api.AddForceAtPosition = physics.AddForceAtPosition;
    api.AddExplosionForce = physics.AddExplosionForce;
    api.SetLinearVelocity = physics.SetLinearVelocity;
    api.GetBodyState = physics.GetBodyState;
    api.GetContactEvents = physics.GetContactEvents;
    api.SphereCast = physics.SphereCast;
    api.OverlapSphere = physics.OverlapSphere;
    // #170 - filtered queries.
    api.RaycastFiltered = [](const float o[3], const float d[3], float maxDist, const QueryFilter& f, RaycastHit& hit) {
        return PhysicsWorld::RaycastFiltered(o, d, maxDist, f, hit);
    };
    api.RaycastAll = [](const float o[3], const float d[3], float maxDist, const QueryFilter& f, RaycastHit* out, int maxHits) {
        return PhysicsWorld::RaycastAll(o, d, maxDist, f, out, maxHits);
    };
    api.SphereCastFiltered = [](const float o[3], const float d[3], float r, float maxDist, const QueryFilter& f,
                                RaycastHit& hit) {
        return PhysicsWorld::SphereCastFiltered(o, d, r, maxDist, f, hit);
    };
    api.BoxCast = [](const float c[3], const float he[3], const float rot[4], const float d[3], float maxDist,
                     const QueryFilter& f, RaycastHit& hit) {
        return PhysicsWorld::BoxCast(c, he, rot, d, maxDist, f, hit);
    };
    api.CapsuleCast = [](const float p1[3], const float p2[3], float r, const float d[3], float maxDist,
                         const QueryFilter& f, RaycastHit& hit) {
        return PhysicsWorld::CapsuleCast(p1, p2, r, d, maxDist, f, hit);
    };
    api.OverlapSphereFiltered = [](const float c[3], float r, const QueryFilter& f, std::uint32_t* out, int maxE) {
        return PhysicsWorld::OverlapSphereFiltered(c, r, f, out, maxE);
    };
    api.OverlapBox = [](const float c[3], const float he[3], const float rot[4], const QueryFilter& f,
                        std::uint32_t* out, int maxE) {
        return PhysicsWorld::OverlapBox(c, he, rot, f, out, maxE);
    };
    // v11
    api.PlaySoundAt = [](const char* path, const float pos[3], float volume, float pitch) {
        if (!path || !*path || !AudioEngine::IsInitialized()) return;
        std::string p = path;
        if (!std::filesystem::path(p).is_absolute()) p = ProjectPaths::Resolve(p);
        static std::unordered_set<std::string> loaded;
        if (!loaded.count(p)) {
            if (!AudioEngine::Load(p)) return;
            loaded.insert(p);
        }
        const AudioEngine::SoundHandle h = AudioEngine::Play(p, std::clamp(volume, 0.0f, 1.0f), false, AudioEngine::Bus::SFX);
        AudioEngine::SetPosition(h, glm::vec3(pos[0], pos[1], pos[2]));
        AudioEngine::SetAttenuation(h, 2.0f, 45.0f);
        AudioEngine::SetPitch(h, std::clamp(pitch, 0.25f, 4.0f));
    };
    api.GetGrabbedEntity = []() -> std::uint32_t { return PhysicsWorld::GrabbedEntity(); };
    api.GetActorPosition = [](std::uint32_t e, float out[3]) { return PhysicsWorld::GetActorPosition(e, out); };
    // v12 - Input Manager (#145)
    api.GetAxis = [](const char* a) { return a ? InputMap::GetAxis(a) : 0.0f; };
    api.GetButton = [](const char* a) { return a && InputMap::GetButton(a); };
    api.GetButtonDown = [](const char* a) { return a && InputMap::GetButtonDown(a); };
    api.GetButtonUp = [](const char* a) { return a && InputMap::GetButtonUp(a); };
    api.GetKey = [](int code) { return InputMap::GetBinding(code); };
    api.GetKeyDown = [](int code) { return InputMap::GetBindingDown(code); };
    api.GetKeyUp = [](int code) { return InputMap::GetBindingUp(code); };
    api.GetMouseDelta = [](float out[2]) {
        const bool on = InputMap::Enabled();
        out[0] = on ? (float)Input::GetMouseDeltaX() : 0.0f;
        out[1] = on ? (float)Input::GetMouseDeltaY() : 0.0f;
    };
    return api;
}

// The candidate / rollback validation: exported entry point, matching API version, an Update.
const GameModuleAPI* ResolveAPI(HMODULE module) {
    const auto getAPI = reinterpret_cast<GetGameModuleAPIFn>(::GetProcAddress(module, "TartarusGetGameModuleAPI"));
    const GameModuleAPI* api = getAPI ? getAPI() : nullptr;
    return (api && api->Version == kGameModuleAPIVersion && api->Update) ? api : nullptr;
}

} // namespace

HotReloadGameModule::~HotReloadGameModule() {
    Shutdown();
}

void HotReloadGameModule::Initialize(const fs::path& sourceModule) {
    Shutdown();
    m_SourceModule = sourceModule;
    m_PollElapsed = 0.0f;
    Reload(true);
}

void HotReloadGameModule::Tick(World& world, float deltaTime, bool playing) {
    m_PollElapsed += deltaTime;
    if (m_PollElapsed >= 0.35f) {
        m_PollElapsed = 0.0f;
        std::error_code ec;
        const fs::file_time_type sourceWrite = fs::last_write_time(m_SourceModule, ec);
        if (!ec && sourceWrite != m_LastSourceWrite && sourceWrite != m_LastFailedSourceWrite)
            Reload(false);
    }

    if (playing && m_API && m_API->Update) m_API->Update(kHostAPI, world, deltaTime);
}

void HotReloadGameModule::FixedTick(World& world, float fixedDeltaTime) {
    if (m_API && m_API->FixedUpdate) m_API->FixedUpdate(kHostAPI, world, fixedDeltaTime);
}

bool HotReloadGameModule::Reload(bool initialLoad) {
    std::error_code ec;
    const fs::file_time_type sourceWrite = fs::last_write_time(m_SourceModule, ec);
    if (ec) {
        if (initialLoad) Log::Warn("Hot reload: TartarusGame.dll was not found; gameplay module is disabled.");
        return false;
    }

    const fs::path cacheDir = fs::temp_directory_path(ec) / "TartarusEngine" / "HotReload";
    if (ec) {
        Log::Error("Hot reload: couldn't resolve a temporary DLL directory.");
        return false;
    }
    fs::create_directories(cacheDir, ec);
    if (ec) {
        Log::Error("Hot reload: couldn't create the temporary DLL directory.");
        return false;
    }

    // On startup, clear numbered copies orphaned by an earlier crash or force-kill (a clean
    // Shutdown deletes its own). Anything still locked by another running instance just fails
    // the remove and is left alone.
    if (initialLoad) {
        std::error_code sweepEc;
        for (const auto& entry : fs::directory_iterator(cacheDir, sweepEc)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"TartarusGame_", 0) == 0 && entry.path().extension() == L".dll") {
                std::error_code rmEc;
                fs::remove(entry.path(), rmEc);
            }
        }
    }

    const fs::path copyPath = cacheDir / ("TartarusGame_" + std::to_string(++m_Generation) + ".dll");
    fs::copy_file(m_SourceModule, copyPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Log::Warn("Hot reload: TartarusGame.dll is still being written; will retry.");
        return false;
    }

    HMODULE candidate = ::LoadLibraryW(copyPath.c_str());
    if (!candidate) {
        Log::Error("Hot reload: couldn't load the rebuilt TartarusGame.dll.");
        fs::remove(copyPath, ec);
        m_LastFailedSourceWrite = sourceWrite; // don't re-attempt this exact build every poll
        return false;
    }

    const GameModuleAPI* candidateAPI = ResolveAPI(candidate);
    if (!candidateAPI) {
        Log::Error("Hot reload: TartarusGame.dll has an incompatible module API.");
        ::FreeLibrary(candidate);
        fs::remove(copyPath, ec);
        m_LastFailedSourceWrite = sourceWrite; // don't re-attempt this exact build every poll
        return false;
    }

    // The candidate is validated; only now touch the live module. HotReloadSwap owns the order
    // (#187): old SaveState -> old OnUnload + free -> new OnLoad(state), with a rollback to the
    // previous build if the new OnLoad rejects itself.
    HotReloadSwap::Slot<GameModuleAPI> live{static_cast<HMODULE>(m_Handle), m_API, m_LoadedCopy};
    const HotReloadSwap::Result result = HotReloadSwap::Swap<GameModuleAPI>(
        live, {candidate, candidateAPI, copyPath}, &ResolveAPI, "Hot reload");
    m_Handle = live.Handle;
    m_API = live.Api;
    m_LoadedCopy = live.Copy;

    if (result != HotReloadSwap::Result::Committed) {
        m_LastFailedSourceWrite = sourceWrite; // this build rejected itself; wait for the next one
        return false;
    }
    m_LastSourceWrite = sourceWrite;
    m_LastFailedSourceWrite = {};
    Log::Info(initialLoad ? "Hot reload: TartarusGame module loaded." : "Hot reload: TartarusGame module reloaded.");
    return true;
}

void HotReloadGameModule::Shutdown() {
    HotReloadSwap::Slot<GameModuleAPI> live{static_cast<HMODULE>(m_Handle), m_API, m_LoadedCopy};
    HotReloadSwap::Unload(live);
    m_Handle = nullptr;
    m_API = nullptr;
    m_LoadedCopy.clear();
}
