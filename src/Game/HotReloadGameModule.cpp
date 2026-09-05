#include "HotReloadGameModule.h"

#include "GameModuleAPI.h"
#include "Log.h"
#include "Model.h"
#include "World.h"
#include "Components.h"

#include <iterator>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

bool HasNamedEntity(const World& world, const char* name) {
    for (auto entity : world.Registry.view<const NameComponent>()) {
        if (world.Registry.get<const NameComponent>(entity).Name == name) return true;
    }
    return false;
}

void EnsureRoomDonutTestSet(World& world) {
    struct DonutSpec {
        const char* Name;
        glm::vec3 Position;
        glm::vec3 Color;
    };
    static const DonutSpec kDonuts[] = {
        {"Hot Reload Donut - Kitchen",     {-3.0f, 0.75f, -3.0f}, {0.95f, 0.45f, 0.22f}},
        {"Hot Reload Donut - Living Room", { 3.0f, 0.75f, -3.0f}, {0.25f, 0.65f, 0.95f}},
        {"Hot Reload Donut - Bathroom",    {-3.0f, 0.75f,  3.0f}, {0.25f, 0.85f, 0.70f}},
        {"Hot Reload Donut - Bedroom",     { 3.0f, 0.75f,  3.0f}, {0.82f, 0.35f, 0.85f}},
    };

    int created = 0;
    for (int i = 0; i < (int)std::size(kDonuts); ++i) {
        const DonutSpec& spec = kDonuts[i];
        if (HasNamedEntity(world, spec.Name)) continue;

        auto model = Model::CreatePrimitive("donut", "primitive://donut#hotreload" + std::to_string(i));
        model->MeshMaterial(0).BaseColor = spec.Color;
        world.CreateModelEntity(model, spec.Position, glm::vec3(0.0f), glm::vec3(1.25f), spec.Name);
        ++created;
    }
    if (created > 0) Log::Info("Hot reload: spawned " + std::to_string(created) + " room donut test object(s).");
}

const GameModuleHostAPI kHostAPI{
    kGameModuleAPIVersion,
    &EnsureRoomDonutTestSet,
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
        if (!ec && sourceWrite != m_LastSourceWrite) Reload(false);
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
        return false;
    }

    const auto getAPI = reinterpret_cast<GetGameModuleAPIFn>(::GetProcAddress(candidate, "TartarusGetGameModuleAPI"));
    const GameModuleAPI* candidateAPI = getAPI ? getAPI() : nullptr;
    if (!candidateAPI || candidateAPI->Version != kGameModuleAPIVersion || !candidateAPI->Update) {
        Log::Error("Hot reload: TartarusGame.dll has an incompatible module API.");
        ::FreeLibrary(candidate);
        fs::remove(copyPath, ec);
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
