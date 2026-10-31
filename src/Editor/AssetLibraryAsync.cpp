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
#include <set>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct AsyncJob {
    enum class Kind { Model, Texture, Material };
    Kind K = Kind::Model;
    std::string Path; // the library key, as requested
    ModelImportSettings ModelSettings;
    TextureImportSettings TexSettings;

    // Worker output, read once Done is set.
    std::shared_ptr<Model> ModelResult;
    TextureCpuData TexResult;
    // A material's texture maps, from its .mat, each with its .meta already read.
    struct MaterialMap {
        std::string Path;
        AssetLibrary::TextureUse Use = AssetLibrary::TextureUse::Color;
        std::string Meta;
    };
    std::vector<MaterialMap> Maps;
    std::atomic<bool> Done{false};

    bool Finalized = false; // main thread: uploaded and in the library (or dropped)
    // Main thread: the load generation it was requested in (AssetLibrary::m_LoadGen), and whether
    // Stop dropped it (a Play load nothing wants any more: its result is discarded on arrival).
    std::uint32_t Gen = 0;
    bool Forget = false;
    // Main thread: jobs this one started when it finished (a material's textures). A ticket
    // waiting on this job waits on these too.
    std::vector<std::shared_ptr<AsyncJob>> Children;

    bool Complete() const {
        if (!Finalized) return false;
        for (const auto& c : Children)
            if (!c->Complete()) return false;
        return true;
    }
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
    std::uint32_t Gen = 0;    // as AsyncJob::Gen
    bool Forget = false;      // Stop dropped it: turns ready without loading its material
};

namespace {
// An edit-mode request (one from before Play) that lands during Play is kept at Stop.
template <class Mark>
void KeepPastPlay(Mark& mark, std::uint32_t gen, std::set<std::string>& into, const std::string& path) {
    if (mark.Active && gen != mark.Gen) into.insert(path);
}

// Worker side of a material request: the .mat's map paths (parsing without a library loads
// nothing) and each map's .meta, so the main thread only files them.
void ReadMaterialMaps(AsyncJob& job) {
    auto ma = MaterialAsset::Load(job.Path, nullptr);
    if (!ma) return;
    using Use = AssetLibrary::TextureUse;
    const std::pair<const std::string*, Use> maps[] = {
        {&ma->AlbedoMapPath, Use::Color},           {&ma->NormalMapPath, Use::Normal},
        {&ma->MetallicRoughnessMapPath, Use::Data}, {&ma->MetallicMapPath, Use::Data},
        {&ma->RoughnessMapPath, Use::Data},         {&ma->AOMapPath, Use::Data},
        {&ma->EmissiveMapPath, Use::Color},         {&ma->ClearCoatMapPath, Use::Data},
        {&ma->ThicknessMapPath, Use::Data},         {&ma->HeightMapPath, Use::Data},
        {&ma->DetailAlbedoMapPath, Use::Color},     {&ma->DetailNormalMapPath, Use::Normal},
        {&ma->GrungeMapPath, Use::Color},
    };
    for (const auto& [p, use] : maps) {
        if (p->empty()) continue;
        AsyncJob::MaterialMap m;
        m.Path = AbsoluteAssetPath(*p);
        m.Use = use;
        m.Meta = AssetDatabase::ReadMetaFields(m.Path);
        job.Maps.push_back(std::move(m));
    }
}
} // namespace

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
                else if (job->K == AsyncJob::Kind::Texture)
                    job->TexResult = Texture::DecodeFile(job->Path, job->TexSettings);
                else
                    ReadMaterialMaps(*job);
            } catch (const std::exception& e) {
                Log::Error("Async load of '" + job->Path + "' failed: " + e.what(), LogContext::Asset(job->Path));
            } catch (...) {
                Log::Error("Async load of '" + job->Path + "' failed.", LogContext::Asset(job->Path));
            }
            job->Done.store(true, std::memory_order_release);
        }
    }
};

AssetLibrary::AssetLibrary() : m_Folders(ProjectSettings::AssetFolders()), m_SavedFolders(m_Folders) {}
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
    if (auto it = a.InFlight.find(key); it != a.InFlight.end()) {
        job = it->second;
        // Asked for again after Stop dropped it: wanted after all, and Stop forgot its settings.
        if (job->Forget) { job->Forget = false; job->Gen = m_LoadGen; PrepareModelImport(path); }
    } else {
        PrepareModelImport(path); // .meta settings and browser data, as LoadModel does first
        job = std::make_shared<AsyncJob>();
        job->Gen = m_LoadGen;
        job->K = AsyncJob::Kind::Model;
        job->Path = path;
        job->ModelSettings = GetModelSettings(path);
        a.InFlight[key] = job;
        a.Active.push_back(job);
        a.Push(job);
    }
    ticket->Gen = m_LoadGen;
    ticket->Jobs.push_back(job);
    a.Tickets.push_back(ticket);
    return ticket;
}

AssetLibrary::AsyncHandle AssetLibrary::RequestTextureAsync(const std::string& path, TextureUse use) {
    return RequestTextureAsync(path, use, nullptr);
}

AssetLibrary::AsyncHandle AssetLibrary::RequestTextureAsync(const std::string& path, TextureUse use,
                                                            const std::string* metaJson) {
    if (path.empty() || HasTexture(path)) return ReadyTicket();
    AsyncLoader& a = Async();
    auto ticket = std::make_shared<AsyncTicket>();
    const std::string key = AssetDatabase::PathKey(path);
    std::shared_ptr<AsyncJob> job;
    if (auto it = a.InFlight.find(key); it != a.InFlight.end()) {
        job = it->second;
        if (job->Forget) { // as RequestModelAsync
            job->Forget = false;
            job->Gen = m_LoadGen;
            ResolveTextureSettings(path, use, metaJson);
            AdoptDiskFolder(path, metaJson);
        }
    } else {
        // As LoadTexture: settings first, and the GUID before the decode, since TextureCache
        // keys its entry by GUID (audit #75).
        ResolveTextureSettings(path, use, metaJson);
        AdoptDiskFolder(path, metaJson);
        AssetDatabase::EnsureGuid(path);
        job = std::make_shared<AsyncJob>();
        job->Gen = m_LoadGen;
        job->K = AsyncJob::Kind::Texture;
        job->Path = path;
        job->TexSettings = GetTextureSettings(path);
        a.InFlight[key] = job;
        a.Active.push_back(job);
        a.Push(job);
    }
    ticket->Gen = m_LoadGen;
    ticket->Jobs.push_back(job);
    a.Tickets.push_back(ticket);
    return ticket;
}

AssetLibrary::AsyncHandle AssetLibrary::RequestMaterialAsync(const std::string& path) {
    if (path.empty() || HasMaterial(path)) return ReadyTicket();
    AsyncLoader& a = Async();
    auto ticket = std::make_shared<AsyncTicket>();
    ticket->MaterialPath = path;
    // The .mat and its maps' .meta files are read on a worker; PumpAsync then requests the maps.
    const std::string key = "mat|" + AssetDatabase::PathKey(path);
    std::shared_ptr<AsyncJob> job;
    if (auto it = a.InFlight.find(key); it != a.InFlight.end()) {
        job = it->second;
        if (job->Forget) { job->Forget = false; job->Gen = m_LoadGen; }
    } else {
        job = std::make_shared<AsyncJob>();
        job->Gen = m_LoadGen;
        job->K = AsyncJob::Kind::Material;
        job->Path = path;
        a.InFlight[key] = job;
        a.Active.push_back(job);
        a.Push(job);
    }
    ticket->Gen = m_LoadGen;
    ticket->Jobs.push_back(job);
    a.Tickets.push_back(ticket);
    return ticket;
}

int AssetLibrary::PumpAsync(double budgetMs) {
    if (!m_Async) return 0;
    AsyncLoader& a = *m_Async;
    const auto start = std::chrono::steady_clock::now();
    bool worked = false;

    std::vector<std::shared_ptr<AsyncJob>> materialsDone; // their maps are requested after the loop
    for (auto it = a.Active.begin(); it != a.Active.end();) {
        if (worked && MsSince(start) >= budgetMs) break;
        AsyncJob& job = **it;
        if (!job.Done.load(std::memory_order_acquire)) { ++it; continue; }

        bool finished = true;
        if (job.K == AsyncJob::Kind::Material) {
            materialsDone.push_back(*it);
        } else if (job.K == AsyncJob::Kind::Model) {
            if (!job.ModelResult || HasModel(job.Path) || job.Forget) {
                // Failed, or a synchronous LoadModel got there first: that one wins. Or Stop
                // dropped it: Play wanted it and Play is over.
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
                    KeepPastPlay(m_PlayMark, job.Gen, m_PlayMark.Models, job.Path);
                }
            }
        } else {
            if (job.TexResult.Ok && !HasTexture(job.Path) && !job.Forget) {
                RegisterTexture(job.Path, std::make_shared<Texture>(std::move(job.TexResult)));
                KeepPastPlay(m_PlayMark, job.Gen, m_PlayMark.Textures, job.Path);
            }
            job.TexResult = TextureCpuData{};
        }
        worked = true;
        if (!finished) break;
        job.Finalized = true;
        a.InFlight.erase(job.K == AsyncJob::Kind::Material ? "mat|" + AssetDatabase::PathKey(job.Path)
                                                           : AssetDatabase::PathKey(job.Path));
        it = a.Active.erase(it);
    }
    for (const auto& m : materialsDone) {
        // The maps are requested in the material's generation, so they share its fate at Stop.
        const std::uint32_t gen = m_LoadGen;
        m_LoadGen = m->Gen;
        for (const auto& map : m->Maps) {
            if (m->Forget) break;
            AsyncHandle t = RequestTextureAsync(map.Path, map.Use, &map.Meta);
            m->Children.insert(m->Children.end(), t->Jobs.begin(), t->Jobs.end());
        }
        m_LoadGen = gen;
        m->Maps.clear();
    }

    // Tickets whose jobs are all in: a material now loads from warm caches (no decode, no GL).
    for (auto it = a.Tickets.begin(); it != a.Tickets.end();) {
        std::shared_ptr<AsyncTicket> t = it->lock();
        if (!t || t->Ready) { it = a.Tickets.erase(it); continue; }
        const bool all = std::all_of(t->Jobs.begin(), t->Jobs.end(),
                                     [](const std::shared_ptr<AsyncJob>& j) { return j->Complete(); });
        if (all) {
            if (!t->MaterialPath.empty() && !t->Forget) {
                LoadMaterial(t->MaterialPath);
                KeepPastPlay(m_PlayMark, t->Gen, m_PlayMark.Materials, t->MaterialPath);
            }
            t->Jobs.clear();
            t->Ready = true;
            it = a.Tickets.erase(it);
        } else {
            ++it;
        }
    }
    return (int)a.Active.size();
}

// --- Play scope (see AssetLibrary.h) -----------------------------------------------------------------
void AssetLibrary::BeginPlayScope() {
    PlayMark& m = m_PlayMark;
    m = PlayMark{};
    m.Active = true;
    m.Gen = ++m_LoadGen;
    for (const auto& [k, v] : m_ModelCache) m.Models.insert(k);
    for (const auto& [k, v] : m_TextureCache) m.Textures.insert(k);
    for (const auto& [k, v] : m_MaterialCache) m.Materials.insert(k);
    m.Sounds.insert(m_Sounds.begin(), m_Sounds.end());
    m.Prefabs.insert(m_Prefabs.begin(), m_Prefabs.end());
    m.Folders.insert(m_Folders.begin(), m_Folders.end());
    for (const auto& [k, v] : m_AssetFolder) m.FolderKeys.insert(k);
    for (const auto& [k, v] : m_DisplayNames) m.NameKeys.insert(k);
    for (const auto& [k, v] : m_Labels) m.LabelKeys.insert(k);
    for (const auto& [k, v] : m_TextureSettings) m.TextureSettingKeys.insert(k);
    for (const auto& [k, v] : m_ModelSettings) m.ModelSettingKeys.insert(k);
}

int AssetLibrary::EndPlayScope() {
    PlayMark& m = m_PlayMark;
    if (!m.Active) return 0;
    m.Active = false;

    // What Play left loading: dropped on arrival unless asked for again (RequestXAsync revives it).
    if (m_Async) {
        for (const auto& job : m_Async->Active)
            if (job->Gen == m.Gen) job->Forget = true;
        for (const auto& w : m_Async->Tickets)
            if (auto t = w.lock(); t && t->Gen == m.Gen) t->Forget = true;
    }
    ++m_LoadGen; // requests from here on are edit mode's again

    int forgot = 0;
    for (const auto& t : std::vector<std::shared_ptr<Texture>>(m_TextureList))
        if (!m.Textures.count(t->Path())) { RemoveTexture(t); ++forgot; }
    for (const auto& mo : std::vector<std::shared_ptr<Model>>(m_ModelList))
        if (!m.Models.count(mo->Path())) { RemoveModel(mo); ++forgot; }
    for (const auto& ma : std::vector<std::shared_ptr<MaterialAsset>>(m_MaterialList))
        if (!m.Materials.count(ma->Path)) { RemoveMaterial(ma); ++forgot; }
    for (const auto& s : std::vector<std::string>(m_Sounds))
        if (!m.Sounds.count(s)) { RemoveSound(s); ++forgot; }
    for (const auto& p : std::vector<std::string>(m_Prefabs))
        if (!m.Prefabs.count(p)) { RemovePrefab(p); ++forgot; }

    // Metadata Play's loads read in (or that requests still in flight prepared). A key new since
    // Play on an asset loaded before it is an Asset Browser edit, already in its .meta: kept.
    std::set<std::string> loaded;
    for (const auto* set : {&m.Models, &m.Textures, &m.Materials, &m.Sounds, &m.Prefabs})
        for (const auto& p : *set) loaded.insert(AssetDatabase::PathKey(p));
    auto prune = [&](auto& map, const std::set<std::string>& before) {
        for (auto it = map.begin(); it != map.end();) {
            if (!before.count(it->first) && !loaded.count(AssetDatabase::PathKey(it->first))) it = map.erase(it);
            else ++it;
        }
    };
    prune(m_AssetFolder, m.FolderKeys);
    prune(m_DisplayNames, m.NameKeys);
    prune(m_Labels, m.LabelKeys);
    prune(m_TextureSettings, m.TextureSettingKeys);
    prune(m_ModelSettings, m.ModelSettingKeys);
    m_AllLabelsDirty = true;

    // Folders adopted from disk for those assets (in memory only, AddFolderPath). One made in the
    // browser during Play is in m_SavedFolders (and settings.json): kept.
    m_Folders.erase(std::remove_if(m_Folders.begin(), m_Folders.end(), [&](const std::string& f) {
        return !m.Folders.count(f) &&
               std::find(m_SavedFolders.begin(), m_SavedFolders.end(), f) == m_SavedFolders.end();
    }), m_Folders.end());

    m = PlayMark{};
    return forgot;
}
