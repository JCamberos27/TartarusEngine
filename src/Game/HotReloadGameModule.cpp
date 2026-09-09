#include "HotReloadGameModule.h"

#include "GameModuleAPI.h"
#include "Log.h"
#include "World.h"
#include "PhysicsWorld.h" // #185 PR 2 — Raycast is backed by the host-side PhysX world

#include <string>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

// The host callback table handed to the game module every Update. The physics callbacks are
// thin forwards to the PhysX world and all no-op (miss / 0 events) whenever it isn't live, so
// a module calling them outside Play just gets nothing back. #185 PR 2 added Raycast, PR 5
// GetTriggerEvents.
const GameModuleHostAPI kHostAPI{
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
};

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

    const auto getAPI = reinterpret_cast<GetGameModuleAPIFn>(::GetProcAddress(candidate, "TartarusGetGameModuleAPI"));
    const GameModuleAPI* candidateAPI = getAPI ? getAPI() : nullptr;
    if (!candidateAPI || candidateAPI->Version != kGameModuleAPIVersion || !candidateAPI->Update) {
        Log::Error("Hot reload: TartarusGame.dll has an incompatible module API.");
        ::FreeLibrary(candidate);
        fs::remove(copyPath, ec);
        m_LastFailedSourceWrite = sourceWrite; // don't re-attempt this exact build every poll
        return false;
    }

    if (candidateAPI->OnLoad) candidateAPI->OnLoad();

    HMODULE previous = static_cast<HMODULE>(m_Handle);
    const fs::path previousCopy = m_LoadedCopy;
    if (m_API && m_API->OnUnload) m_API->OnUnload();
    if (previous) ::FreeLibrary(previous);
    if (!previousCopy.empty()) fs::remove(previousCopy, ec);

    m_Handle = candidate;
    m_API = candidateAPI;
    m_LoadedCopy = copyPath;
    m_LastSourceWrite = sourceWrite;
    m_LastFailedSourceWrite = {};
    Log::Info(initialLoad ? "Hot reload: TartarusGame module loaded." : "Hot reload: TartarusGame module reloaded.");
    return true;
}

void HotReloadGameModule::Shutdown() {
    if (m_API && m_API->OnUnload) m_API->OnUnload();
    if (m_Handle) ::FreeLibrary(static_cast<HMODULE>(m_Handle));

    std::error_code ec;
    if (!m_LoadedCopy.empty()) fs::remove(m_LoadedCopy, ec);
    m_Handle = nullptr;
    m_API = nullptr;
    m_LoadedCopy.clear();
}
