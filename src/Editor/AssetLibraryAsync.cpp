// AssetLibrary's async loading (RequestModelAsync / RequestMaterialAsync / RequestTextureAsync /
// PumpAsync). Loading an asset used to stall the main thread for the whole Assimp import and every
// texture decode - a Character Outfit change (a few FBX files, a dozen 4K PNGs) froze the editor
// for seconds. The CPU work now runs on worker threads, and the main thread only does the GL
// uploads, a few milliseconds' worth per frame.
//
// Threading contract:
// - Workers run Model::ImportDeferred and Texture::DecodeFile, which touch no GL and none of
//   AssetLibrary's maps (AssetDatabase, Log and TextureCache are thread-safe on their own).
// - Everything else - settings and .meta reads, cache lookups and inserts, GL - is main-thread.
// - A job's result is written by one worker and read by the main thread only after `Done` is set
//   (release/acquire), so it needs no lock.
#include "AssetLibrary.h"
#include "AssetDatabase.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "ProjectSettings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct AsyncJob {
    enum class Kind { Model, Texture };
    Kind K = Kind::Model;
    std::string Path; // the library key, as requested
    ModelImportSettings ModelSettings;
    TextureImportSettings TexSettings;

    // Worker output, read once Done is set.
    std::shared_ptr<Model> ModelResult;
    TextureCpuData TexResult;
    std::atomic<bool> Done{false};

    bool Finalized = false; // main thread: uploaded and in the library (or dropped)
};

// A material's texture paths are absolute or project-relative (MaterialAsset.cpp's rule).
std::string AbsoluteAssetPath(const std::string& p) {
    if (p.empty() || std::filesystem::path(p).is_absolute()) return p;
    return ProjectPaths::Resolve(p);
}

double MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

class AssetLibrary::AsyncTicket {
public:
    std::vector<std::shared_ptr<AsyncJob>> Jobs;
    std::string MaterialPath; // loaded from the (now warm) caches once every job is finalized
    bool Ready = false;
};

struct AssetLibrary::AsyncLoader {
    std::mutex Mutex;
    std::condition_variable Wake;
    std::deque<std::shared_ptr<AsyncJob>> Queue;
    bool Stop = false;
    std::vector<std::thread> Workers;

    // Main thread only.
    std::map<std::string, std::shared_ptr<AsyncJob>> InFlight; // PathKey -> unfinalized job
    std::vector<std::shared_ptr<AsyncJob>> Active;             // unfinalized, in request order
    std::vector<std::weak_ptr<AsyncTicket>> Tickets;           // not ready yet

    AsyncLoader() {
        // The decode side needs the driver's compressed formats known before any worker asks.
        Texture::InitGpuCaps();
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        const unsigned n = std::clamp(hw / 2, 2u, 4u);
        for (unsigned i = 0; i < n; ++i) Workers.emplace_back([this] { Run(); });
    }

    ~AsyncLoader() {
        {
            std::lock_guard<std::mutex> lk(Mutex);
            Stop = true;
            Queue.clear();
        }
        Wake.notify_all();
        for (auto& t : Workers) t.join();
        // Results still here are dropped on the main thread (the one destroying the library), so
        // the few that did get GL objects free them on the right thread.
    }

    void Push(const std::shared_ptr<AsyncJob>& job) {
        {
            std::lock_guard<std::mutex> lk(Mutex);
            Queue.push_back(job);
        }
        Wake.notify_one();
    }

    void Run() {
        for (;;) {
            std::shared_ptr<AsyncJob> job;
            {
                std::unique_lock<std::mutex> lk(Mutex);
                Wake.wait(lk, [this] { return Stop || !Queue.empty(); });
                if (Stop) return;
                job = std::move(Queue.front());
                Queue.pop_front();
            }
            try {
                if (job->K == AsyncJob::Kind::Model)
                    job->ModelResult = Model::ImportDeferred(job->Path, job->ModelSettings);
                else
                    job->TexResult = Texture::DecodeFile(job->Path, job->TexSettings);
            } catch (const std::exception& e) {
                Log::Error("Async load of '" + job->Path + "' failed: " + e.what(), LogContext::Asset(job->Path));
            } catch (...) {
                Log::Error("Async load of '" + job->Path + "' failed.", LogContext::Asset(job->Path));
            }
            job->Done.store(true, std::memory_order_release);
        }
    }
};

AssetLibrary::AssetLibrary() : m_Folders(ProjectSettings::AssetFolders()) {}
AssetLibrary::~AssetLibrary() = default;

AssetLibrary::AsyncLoader& AssetLibrary::Async() {
    if (!m_Async) m_Async = std::make_unique<AsyncLoader>();
    return *m_Async;
}

bool AssetLibrary::IsReady(const AsyncHandle& ticket) { return !ticket || ticket->Ready; }

int AssetLibrary::AsyncInFlight() const {
    if (!m_Async) return 0;
    return (int)m_Async->Active.size();
}

namespace {
AssetLibrary::AsyncHandle ReadyTicket() {
    auto t = std::make_shared<AssetLibrary::AsyncTicket>();
    t->Ready = true;
    return t;
}
} // namespace

AssetLibrary::AsyncHandle AssetLibrary::RequestModelAsync(const std::string& path) {
    // Primitives are generated, not imported: nothing to wait for.
    if (path.empty() || path.rfind("primitive://", 0) == 0 || HasModel(path)) return ReadyTicket();
    AsyncLoader& a = Async();
    auto ticket = std::make_shared<AsyncTicket>();
    const std::string key = AssetDatabase::PathKey(path);
    std::shared_ptr<AsyncJob> job;
    if (auto it = a.InFlight.find(key); it != a.InFlight.end()) job = it->second;
    else {
        PrepareModelImport(path); // .meta settings and browser data, as LoadModel does first
        job = std::make_shared<AsyncJob>();
        job->K = AsyncJob::Kind::Model;
        job->Path = path;
        job->ModelSettings = GetModelSettings(path);
        a.InFlight[key] = job;
        a.Active.push_back(job);
        a.Push(job);
    }
    ticket->Jobs.push_back(job);
    a.Tickets.push_back(ticket);
    return ticket;
}

AssetLibrary::AsyncHandle AssetLibrary::RequestTextureAsync(const std::string& path, TextureUse use) {
    if (path.empty() || HasTexture(path)) return ReadyTicket();
    AsyncLoader& a = Async();
    auto ticket = std::make_shared<AsyncTicket>();
    const std::string key = AssetDatabase::PathKey(path);
    std::shared_ptr<AsyncJob> job;
    if (auto it = a.InFlight.find(key); it != a.InFlight.end()) job = it->second;
    else {
        // As LoadTexture: settings first, and the GUID before the decode, since TextureCache
        // keys its entry by GUID (audit #75).
        ResolveTextureSettings(path, use);
        AdoptDiskFolder(path);
        AssetDatabase::EnsureGuid(path);
        job = std::make_shared<AsyncJob>();
        job->K = AsyncJob::Kind::Texture;
        job->Path = path;
        job->TexSettings = GetTextureSettings(path);
        a.InFlight[key] = job;
        a.Active.push_back(job);
        a.Push(job);
    }
    ticket->Jobs.push_back(job);
    a.Tickets.push_back(ticket);
    return ticket;
}

AssetLibrary::AsyncHandle AssetLibrary::RequestMaterialAsync(const std::string& path) {
    if (path.empty() || HasMaterial(path)) return ReadyTicket();
    AsyncLoader& a = Async();
    auto ticket = std::make_shared<AsyncTicket>();
    ticket->MaterialPath = path;
    // Parsing without a library reads the .mat's settings and map paths and loads nothing.
    if (auto ma = MaterialAsset::Load(path, nullptr)) {
        const std::pair<const std::string*, TextureUse> maps[] = {
            {&ma->AlbedoMapPath, TextureUse::Color},       {&ma->NormalMapPath, TextureUse::Normal},
            {&ma->MetallicRoughnessMapPath, TextureUse::Data}, {&ma->MetallicMapPath, TextureUse::Data},
            {&ma->RoughnessMapPath, TextureUse::Data},     {&ma->AOMapPath, TextureUse::Data},
            {&ma->EmissiveMapPath, TextureUse::Color},     {&ma->ClearCoatMapPath, TextureUse::Data},
            {&ma->ThicknessMapPath, TextureUse::Data},     {&ma->HeightMapPath, TextureUse::Data},
            {&ma->DetailAlbedoMapPath, TextureUse::Color}, {&ma->DetailNormalMapPath, TextureUse::Normal},
        };
        for (const auto& [p, use] : maps) {
            if (p->empty()) continue;
            AsyncHandle tex = RequestTextureAsync(AbsoluteAssetPath(*p), use);
            ticket->Jobs.insert(ticket->Jobs.end(), tex->Jobs.begin(), tex->Jobs.end());
        }
    }
    a.Tickets.push_back(ticket);
    return ticket;
}

int AssetLibrary::PumpAsync(double budgetMs) {
    if (!m_Async) return 0;
    AsyncLoader& a = *m_Async;
    const auto start = std::chrono::steady_clock::now();
    bool worked = false;

    for (auto it = a.Active.begin(); it != a.Active.end();) {
        if (worked && MsSince(start) >= budgetMs) break;
        AsyncJob& job = **it;
        if (!job.Done.load(std::memory_order_acquire)) { ++it; continue; }

        bool finished = true;
        if (job.K == AsyncJob::Kind::Model) {
            if (!job.ModelResult || HasModel(job.Path)) {
                // Failed, or a synchronous LoadModel got there first: that one wins.
                job.ModelResult.reset();
            } else {
                // One mesh or texture per step, re-checking the budget between them.
                while (!job.ModelResult->FinishGpuUploadStep()) {
                    worked = true;
                    if (MsSince(start) >= budgetMs) { finished = false; break; }
                }
                if (finished) {
                    if (job.ModelResult->MeshCount() == 0 && !job.ModelResult->HasAnimations())
                        Log::Error("Model: async import of '" + job.Path + "' produced nothing.", LogContext::Asset(job.Path));
                    RegisterModel(job.Path, std::move(job.ModelResult));
                }
            }
        } else {
            if (job.TexResult.Ok && !HasTexture(job.Path))
                RegisterTexture(job.Path, std::make_shared<Texture>(std::move(job.TexResult)));
            job.TexResult = TextureCpuData{};
        }
        worked = true;
        if (!finished) break;
        job.Finalized = true;
        a.InFlight.erase(AssetDatabase::PathKey(job.Path));
        it = a.Active.erase(it);
    }

    // Tickets whose jobs are all in: a material now loads from warm caches (no decode, no GL).
    for (auto it = a.Tickets.begin(); it != a.Tickets.end();) {
        std::shared_ptr<AsyncTicket> t = it->lock();
        if (!t || t->Ready) { it = a.Tickets.erase(it); continue; }
        const bool all = std::all_of(t->Jobs.begin(), t->Jobs.end(),
                                     [](const std::shared_ptr<AsyncJob>& j) { return j->Finalized; });
        if (all) {
            if (!t->MaterialPath.empty()) LoadMaterial(t->MaterialPath);
            t->Jobs.clear();
            t->Ready = true;
            it = a.Tickets.erase(it);
        } else {
            ++it;
        }
    }
    return (int)a.Active.size();
}
