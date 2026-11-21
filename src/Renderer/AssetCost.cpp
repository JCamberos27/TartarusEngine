#include "AssetCost.h"

#include "Core/Env.h"
#include "Game/ComponentsCore.h"
#include "Game/ComponentsRender.h"
#include "Game/World.h"
#include "Model.h"
#include "ModelMesh.h"
#include "SceneViewPass.h"
#include "Texture.h"
#include "TimeService.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace {

// One model at one LOD level in one view: triangles drawn this frame (Cur), over the phase (Sum) and in its worst
// frame (Max), and how many times it was drawn.
struct DrawRow {
    std::string Path;
    int Lod = -1;
    long long Sum = 0, Cur = 0, Max = 0, Draws = 0;
    int TrisEach = 0;
    const Model* M = nullptr;
};

struct Tally {
    std::unordered_map<std::string, DrawRow> Rows;
    std::uint64_t LastFrame = ~0ull;
    int Frames = 0;
    // The frame just ended: its per-frame totals feed the worst frame.
    void Fold() {
        for (auto& [k, r] : Rows) { r.Max = std::max(r.Max, r.Cur); r.Cur = 0; }
    }
    void Clear() { Rows.clear(); LastFrame = ~0ull; Frames = 0; }
};

struct LoadRow { double Ms = 0.0; int Count = 0; std::string Kind, Name; };

struct State {
    Tally Views[2]; // 0 = the Scene tab, 1 = the Game view
    std::unordered_map<std::string, LoadRow> Loads; // by NormPath
    std::unordered_set<std::uint32_t> Seen; // NoteView's per-call de-dupe (an item in both passes)
};

State& S() {
    static State s;
    return s;
}

// The join key between draw / texture paths and the load log: lower case, forward slashes.
std::string NormPath(const std::string& p) {
    std::string out = p;
    for (char& c : out) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    return out;
}

std::string CsvField(const std::string& s) {
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// GPU bytes of a model's meshes (vertices + 32-bit indices), as MemReport counts them.
double MeshMiB(const Model* m) {
    if (!m) return 0.0;
    size_t bytes = 0;
    for (int i = 0; i < m->MeshCount(); ++i)
        if (const ModelMesh* mm = m->MeshPtr(i); mm && mm->VertexBuffer() != 0)
            bytes += (size_t)mm->VertexCount() * sizeof(ModelVertex) + (size_t)mm->IndexCount() * 4;
    return bytes / (1024.0 * 1024.0);
}

} // namespace

namespace AssetCost {

bool Enabled() {
    static const bool on = !Env::Str(Env::Var::AssetCost).empty();
    return on;
}

int LodLevelOf(const World& world, unsigned entity) {
    const auto& reg = world.Registry;
    entt::entity e = (entt::entity)entity;
    for (int depth = 0; depth < 64 && reg.valid(e); ++depth) {
        const auto* h = reg.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null || !reg.valid(h->Parent)) return -1;
        if (reg.all_of<LODGroupComponent>(h->Parent)) {
            const auto* ph = reg.try_get<HierarchyComponent>(h->Parent);
            if (!ph) return -1;
            const auto it = std::find(ph->Children.begin(), ph->Children.end(), e);
            const int idx = (int)(it - ph->Children.begin());
            return idx < 4 && it != ph->Children.end() ? idx : -1;
        }
        e = h->Parent;
    }
    return -1;
}

void NoteView(World& world, bool editorView, std::initializer_list<const std::vector<SceneDrawItem>*> lists) {
    State& s = S();
    Tally& t = s.Views[editorView ? 0 : 1];
    const std::uint64_t frame = Time::FrameCount();
    if (frame != t.LastFrame) {
        if (t.LastFrame != ~0ull) t.Fold();
        t.LastFrame = frame;
        ++t.Frames;
    }
    s.Seen.clear();
    for (const std::vector<SceneDrawItem>* list : lists) {
        for (const SceneDrawItem& it : *list) {
            if (!it.Ref) continue;
            // A model with opaque and transparent slots is queued in both passes, each item carrying its full count.
            if (it.SortId != 0xFFFFFFFFu && !s.Seen.insert(it.SortId).second) continue;
            const int lod = it.SortId != 0xFFFFFFFFu ? LodLevelOf(world, it.SortId) : -1;
            std::string key = it.Ref->Path();
            key += '|';
            key += std::to_string(lod);
            DrawRow& r = t.Rows[key];
            if (r.Path.empty()) { r.Path = it.Ref->Path(); r.Lod = lod; r.M = it.Ref; }
            r.TrisEach = std::max(r.TrisEach, it.Tris);
            r.Cur += it.Tris;
            r.Sum += it.Tris;
            ++r.Draws;
        }
    }
}

void NoteLoads(const std::vector<FrameLoads::Event>& events) {
    State& s = S();
    for (const FrameLoads::Event& e : events) {
        LoadRow& r = s.Loads[NormPath(e.Name)];
        if (r.Name.empty()) { r.Name = e.Name; r.Kind = e.Kind; }
        else if (r.Kind.find(e.Kind) == std::string::npos) { r.Kind += " + "; r.Kind += e.Kind; }
        r.Ms += e.Ms;
        ++r.Count;
    }
}

void WritePhase(const std::string& sceneFile, const char* mode) {
    State& s = S();
    const std::string path = Env::Str(Env::Var::AssetCost);
    const bool edit = std::string(mode) == "edit";
    Tally& t = s.Views[edit ? 0 : 1];
    t.Fold();

    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
    std::ofstream out(path, std::ios::app);
    if (!out) {
        std::printf("[AssetCost] cannot write '%s'\n", path.c_str());
    } else {
        if (fresh)
            out << "scene,phase,view,kind,path,lod,frames,avg_tris,max_tris,avg_draws,tris_each,gpu_mib,format,width,"
                   "height,mips,load_ms,loads,load_kind\n";
        const char* view = edit ? "scene-tab" : "game-view";
        std::unordered_set<std::string> listed; // NormPath of every model / texture row: the rest go out as "load" rows
        auto loadCols = [&](const std::string& p) {
            char buf[64];
            const auto it = s.Loads.find(NormPath(p));
            if (it == s.Loads.end()) return std::string(",,");
            std::snprintf(buf, sizeof(buf), "%.3f,%d,", it->second.Ms, it->second.Count);
            return buf + CsvField(it->second.Kind);
        };
        auto lead = [&](const char* kind, const std::string& p) {
            return CsvField(sceneFile) + ',' + mode + ',' + view + ',' + kind + ',' + CsvField(p) + ',';
        };

        // Models, by triangles drawn per frame.
        std::vector<const DrawRow*> rows;
        for (const auto& [k, r] : t.Rows) rows.push_back(&r);
        std::sort(rows.begin(), rows.end(), [](const DrawRow* a, const DrawRow* b) {
            return a->Sum != b->Sum ? a->Sum > b->Sum : a->Path != b->Path ? a->Path < b->Path : a->Lod < b->Lod;
        });
        const double frames = std::max(t.Frames, 1);
        char buf[256];
        for (const DrawRow* r : rows) {
            std::snprintf(buf, sizeof(buf), "%d,%d,%.1f,%lld,%.2f,%d,%.3f,,,,,", r->Lod, t.Frames, r->Sum / frames,
                          r->Max, r->Draws / frames, r->TrisEach, MeshMiB(r->M));
            out << lead("model", r->Path) << buf << loadCols(r->Path) << '\n';
            listed.insert(NormPath(r->Path));
        }

        // Live textures (editor icons included), by GPU bytes.
        struct TexRow { std::string Path, Format; double MiB; int W, H; bool Mips; };
        std::vector<TexRow> texes;
        Texture::ForEachLive([&](const Texture& tex) {
            texes.push_back({tex.Path(), tex.GpuFormatName() ? tex.GpuFormatName() : "", tex.GpuBytes() / (1024.0 * 1024.0),
                             tex.UploadWidth(), tex.UploadHeight(), tex.ImportSettings().GenerateMipmaps});
        });
        std::sort(texes.begin(), texes.end(), [](const TexRow& a, const TexRow& b) {
            return a.MiB != b.MiB ? a.MiB > b.MiB : a.Path < b.Path;
        });
        for (const TexRow& tr : texes) {
            std::snprintf(buf, sizeof(buf), ",,,,,,%.3f,", tr.MiB);
            out << lead("texture", tr.Path) << buf << CsvField(tr.Format) << ',' << tr.W << ',' << tr.H << ','
                << (tr.Mips ? 1 : 0) << ',' << loadCols(tr.Path) << '\n';
            listed.insert(NormPath(tr.Path));
        }

        // Everything else the scene loaded (shaders, models not drawn in this view), by load time.
        std::vector<const LoadRow*> rest;
        for (const auto& [k, r] : s.Loads)
            if (!listed.count(k)) rest.push_back(&r);
        std::sort(rest.begin(), rest.end(), [](const LoadRow* a, const LoadRow* b) {
            return a->Ms != b->Ms ? a->Ms > b->Ms : a->Name < b->Name;
        });
        for (const LoadRow* r : rest) {
            std::snprintf(buf, sizeof(buf), ",,,,,,,,,,,%.3f,%d,", r->Ms, r->Count);
            out << lead("load", r->Name) << buf << CsvField(r->Kind) << '\n';
        }
        std::printf("[AssetCost] %s (%s): %zu model rows, %zu textures, %zu other loads -> %s\n", sceneFile.c_str(), mode,
                    rows.size(), texes.size(), rest.size(), path.c_str());
    }
    t.Clear();
    s.Views[edit ? 1 : 0].Clear(); // the other view's frames belong to no phase
    if (std::string(mode) == "play-max") s.Loads.clear(); // the scene's last phase: the next scene counts its own
}

} // namespace AssetCost
