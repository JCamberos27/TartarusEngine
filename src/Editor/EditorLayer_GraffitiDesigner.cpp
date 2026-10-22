// The Graffiti Designer (decals Phase 3, docs/GRAFFITI_DESIGNER.md): the panel that edits a .graffiti
// recipe - a layer stack (fill, stamp, text, spray) composited on the GPU (src/Graffiti) - and bakes
// it into the DA_Piece_<name>.decal the decal renderer draws; plus spraying straight onto a wall in
// the Scene view, which creates a decal with a fresh piece and records each stroke into it.
//
// Undo: every recipe write goes through AtomicFile, so the editor's file journal makes each edit (a
// stroke, a drag, a typing session) one undo step; undo rewrites the file, and the Designer notices
// the new write time, reloads, and re-bakes a piece that's on a wall. A bake's worker writes the maps
// off-thread (outside the journal), so the Designer hands the pre-images over when it finishes.
#include "EditorLayer.h"
#include "EditorGraffitiDesigner.h"
#include "EditorLayerInternal.h"
#include "EditorPanels.h"
#include "EditorSound.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorTheme.h"
#include "Glass/GlassMotion.h"
#include "Glass/GlassTokens.h"
#include "AssetDatabase.h"
#include "AssetThumbnailLoader.h"
#include "EditorTestProbe.h"
#include "Camera.h"
#include "PenInput.h"
#include "Components.h"
#include "DecalAsset.h"
#include "DecalSystem.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "World.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace EditorInternal;
using GTool = GraffitiDesigner::Tool;

namespace {

constexpr const char* kPiecesFolder = "assets/Decals/Graffiti/Pieces";
constexpr float kRowH = 26.0f;

long long FileStamp(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(fs::u8path(path), ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}

std::string Abs(const std::string& p) {
    if (p.empty()) return {};
    return fs::u8path(p).is_absolute() ? p : ProjectPaths::Resolve(p);
}

double Now() { return ImGui::GetTime(); }

const char* ToolIcon(GTool t) {
    switch (t) {
        case GTool::Move: return ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT;
        case GTool::Stamp: return ICON_FA_STAMP;
        case GTool::Text: return ICON_FA_FONT;
        case GTool::Spray: return ICON_FA_SPRAY_CAN;
        case GTool::Eyedropper: return ICON_FA_EYE_DROPPER;
        case GTool::Erase: return ICON_FA_ERASER;
        default: return "?";
    }
}
const char* ToolName(GTool t) {
    static const char* n[] = {"Move", "Stamp", "Text", "Spray", "Eyedropper", "Erase"};
    return n[std::clamp((int)t, 0, 5)];
}
const char* LayerIcon(Graffiti::LayerType t) {
    switch (t) {
        case Graffiti::LayerType::Fill: return ICON_FA_FILL_DRIP;
        case Graffiti::LayerType::Stamp: return ICON_FA_STAMP;
        case Graffiti::LayerType::Text: return ICON_FA_FONT;
        default: return ICON_FA_SPRAY_CAN;
    }
}

// FNV-1a over the four baked PNGs: equal bakes hash equal (the smoke test's determinism check).
uint64_t HashFiles(const Graffiti::BakeFiles& f) {
    uint64_t h = 1469598103934665603ull;
    for (const std::string* s : {&f.Albedo, &f.Normal, &f.Surface, &f.Aux})
        for (unsigned char c : *s) h = (h ^ c) * 1099511628211ull;
    return h;
}

void SyncLayerIds(GraffitiDesigner& g) {
    const size_t n = g.Piece ? g.Piece->Layers.size() : 0;
    while (g.LayerIds.size() < n) g.LayerIds.push_back(g.NextLayerId++);
    g.LayerIds.resize(n);
}

void PushRecent(GraffitiDesigner& g, const glm::vec3& c) {
    const glm::vec3 q = Graffiti::Quantise(c);
    g.Recent.erase(std::remove_if(g.Recent.begin(), g.Recent.end(), [&](const glm::vec3& r) { return glm::all(glm::equal(r, q)); }),
                   g.Recent.end());
    g.Recent.insert(g.Recent.begin(), q);
    if (g.Recent.size() > 8) g.Recent.resize(8);
}

// The program index of piece layer `layer` (programs hold the visible layers only), -1 when hidden.
int ProgramIndex(const GraffitiDesigner& g, int layer) {
    for (int i = 0; i < (int)g.Program.Layers.size(); ++i)
        if (g.Program.Layers[(size_t)i].Layer == layer) return i;
    return -1;
}

// Rebuilds the paint program and re-renders the canvas. The text cascade (letters typed in the last
// moment) scales its new letters in; a bake always builds its own program without it.
void Rebuild(GraffitiDesigner& g) {
    if (!g.Piece) return;
    g.Program = Graffiti::Build(*g.Piece, g.Comp.MetricsCallback());
    bool cascading = false;
    if (g.CascadeLayer >= 0 && !Glass::Motion().ReduceMotion()) {
        const float t = (float)(Now() - g.CascadeStart);
        const int pi = ProgramIndex(g, g.CascadeLayer);
        if (pi >= 0) {
            auto& quads = g.Program.Layers[(size_t)pi].Quads;
            for (int k = g.CascadeFrom; k < (int)quads.size(); ++k) {
                const float r = std::clamp((t - (float)(k - g.CascadeFrom) * 0.035f) / 0.22f, 0.0f, 1.0f);
                quads[(size_t)k].Reveal = 1.0f - (1.0f - r) * (1.0f - r) * (1.0f - r); // out-cubic
                cascading |= r < 1.0f;
            }
        }
    }
    if (!cascading) g.CascadeLayer = -1;
    g.Comp.Render(*g.Piece, g.Program);
    g.NeedsBuild = cascading; // keep animating while letters are still landing
}

void SaveNow(GraffitiDesigner& g) {
    if (!g.Piece) return;
    if (!g.Piece->Save()) Log::Error("Graffiti: couldn't save '" + g.Piece->Path + "'.", LogContext::Asset(g.Piece->Path));
    g.Stamp = FileStamp(g.Piece->Path);
    g.Dirty = false;
}

void Edited(GraffitiDesigner& g) {
    g.Dirty = true;
    g.NeedsBuild = true;
}

// Bakes right now, waiting for the GPU (a new piece needs its .decal before a decal can show it).
bool BakeNow(GraffitiDesigner& g) {
    if (!g.Piece) return false;
    const Graffiti::Program prog = Graffiti::Build(*g.Piece, g.Comp.MetricsCallback());
    g.Comp.StartBake(*g.Piece, prog);
    Graffiti::BakeImages img;
    if (!g.Comp.PollBake(img, /*wait=*/true)) return false;
    const Graffiti::BakeFiles files = Graffiti::EncodePngs(img);
    if (!Graffiti::WriteBake(*g.Piece, files)) return false;
    g.LastBakeHash = HashFiles(files);
    ++g.Bakes;
    g.BakeDoneAt = Now();
    g.NeedsBuild = true; // the bake rendered without the cascade: show the canvas again
    if (DecalSystem* ds = DecalSystem::Current()) ds->Refresh(g.Piece->DecalPath());
    return true;
}

// `undoable`: the Bake button's bake is an undo step of its own; the automatic ones (after a stroke, an
// undo) are just the recipe's picture catching up and ride its steps.
void StartBake(GraffitiDesigner& g, bool undoable = false) {
    if (!g.Piece) return;
    if (g.BakeStage != 0 || g.Stroking) {
        g.BakeAgain = true;
        g.BakeAgainUndoable = g.BakeAgainUndoable || undoable;
        return;
    }
    g.BakeUndoable = undoable;
    if (g.Dirty) SaveNow(g);
    const Graffiti::Program prog = Graffiti::Build(*g.Piece, g.Comp.MetricsCallback());
    g.Comp.StartBake(*g.Piece, prog);
    g.BakeStage = 1;
    g.Status = "Baking...";
    EditorSound::Play(EditorSound::Event::GraffitiBakeStart);
}

std::shared_ptr<Graffiti::Piece> NewPiece(const std::string& path) {
    auto p = std::make_shared<Graffiti::Piece>();
    p->Path = path;
    std::string stem = fs::u8path(path).stem().u8string();
    p->Name = stem.rfind("GP_", 0) == 0 ? stem.substr(3) : stem;
    p->Seed = (uint32_t)std::hash<std::string>{}(path) & 0x7FFFFFFFu;
    Graffiti::Layer spray;
    spray.Type = Graffiti::LayerType::Spray;
    spray.Name = "Spray";
    p->Layers.push_back(spray);
    return p;
}

// The spray layer a stroke goes into: the selected one when it is a spray layer, else a new one just
// above the selection.
int EnsureSprayLayer(GraffitiDesigner& g) {
    auto& L = g.Piece->Layers;
    if (g.Selected >= 0 && g.Selected < (int)L.size() && L[(size_t)g.Selected].Type == Graffiti::LayerType::Spray) {
        L[(size_t)g.Selected].Visible = true;
        return g.Selected;
    }
    Graffiti::Layer spray;
    spray.Type = Graffiti::LayerType::Spray;
    spray.Name = "Spray " + std::to_string(L.size() + 1);
    const int at = g.Selected >= 0 && g.Selected < (int)L.size() ? g.Selected + 1 : (int)L.size();
    L.insert(L.begin() + at, spray);
    g.LayerIds.insert(g.LayerIds.begin() + at, g.NextLayerId++);
    g.Selected = at;
    EditorSound::Play(EditorSound::Event::GraffitiLayerAdd);
    Edited(g);
    return at;
}

// --- Live strokes ------------------------------------------------------------------------------------

void DrainDue(GraffitiDesigner& g, double until, bool all) {
    if (g.Pending.empty()) return;
    std::vector<size_t> due;
    for (size_t i = 0; i < g.Pending.size(); ++i)
        if (all || g.Pending[i].Time <= (float)until) due.push_back(i);
    if (due.empty()) return;
    std::sort(due.begin(), due.end(), [&](size_t a, size_t b) {
        return g.Pending[a].Time < g.Pending[b].Time || (g.Pending[a].Time == g.Pending[b].Time && g.PendingSeq[a] < g.PendingSeq[b]);
    });
    std::vector<Graffiti::Splat> out;
    out.reserve(due.size());
    bool drip = false;
    for (size_t i : due) {
        out.push_back(g.Pending[i]);
        drip |= (g.Pending[i].Flags & Graffiti::kSplatDrip) != 0;
    }
    size_t keep = 0;
    for (size_t i = 0; i < g.Pending.size(); ++i)
        if (!(all || g.Pending[i].Time <= (float)until)) {
            g.Pending[keep] = g.Pending[i];
            g.PendingSeq[keep++] = g.PendingSeq[i];
        }
    g.Pending.resize(keep);
    g.PendingSeq.resize(keep);
    g.Comp.LiveSplats(out.data(), out.size());
    if (drip && Now() - g.LastDripSound > 0.3) {
        g.LastDripSound = Now();
        EditorSound::Play(EditorSound::Event::SprayDrip);
    }
}

// A pen's pressure scales the Pressure slider (the slider is the heaviest the can goes); with no pen
// the slider's value stands and the pointer-speed easing applies.
float PenPressure(const GraffitiDesigner& g) {
    const float pen = PenInput::Pressure();
    return pen >= 0.0f ? g.Pressure * std::max(pen, 0.05f) : -1.0f;
}

void BeginStroke(GraffitiDesigner& g, glm::vec2 uv, bool erase, bool inViewport) {
    if (!g.Piece) return;
    const int layer = EnsureSprayLayer(g);
    if (g.NeedsBuild || g.Dirty) Rebuild(g); // the live layer must be in the program
    g.CascadeLayer = -1;
    g.Stroking = true;
    g.StrokeInViewport = inViewport;
    g.StrokeLayer = layer;
    g.Live = Graffiti::Stroke{};
    g.Live.Tip = g.Cap;
    g.Live.Color = Graffiti::Quantise(g.Color);
    g.Live.Erase = erase;
    g.Live.Flow = g.Flow;
    g.Live.Angle = g.NibAngle;
    g.StrokeStart = Now();
    g.LastSampleAt = -1.0;
    g.LastUv = uv;
    g.LastMouse = ImGui::GetIO().MousePos;
    const float pen0 = PenPressure(g);
    const float start = pen0 >= 0.0f ? pen0 : g.Pressure;
    g.LivePressure = start;
    g.Pending.clear();
    g.PendingSeq.clear();
    g.NextSeq = 0;
    g.Live.Samples.push_back(Graffiti::SpraySample::Make(uv, start, g.DistanceCm * 0.01f, 0.0f));
    const Graffiti::Piece& p = *g.Piece;
    const uint32_t index = (uint32_t)p.Layers[(size_t)layer].Strokes.size();
    g.Builder.Begin(g.Live, p.Width, p.Height, (float)p.Width / std::max(p.Metres.x, 1e-3f),
                    Graffiti::StrokeSeed(p.Seed, g.Live, index), p.Drips);
    std::vector<Graffiti::Splat> none;
    g.Builder.Add(g.Live.Samples.back(), none);
    g.Comp.BeginLive(p, g.Program, std::max(ProgramIndex(g, layer), 0));
    EditorSound::Play(EditorSound::Event::SprayStart);
    g.Hiss = EditorSound::StartLoop(EditorSound::Event::SprayLoop, 0.4f + 0.6f * start, 0.9f + 0.2f * start);
    PushRecent(g, g.Color);
}

void StrokeSample(GraffitiDesigner& g, glm::vec2 uv) {
    if (!g.Stroking) return;
    const double now = Now();
    const double t = now - g.StrokeStart;
    if (g.LastSampleAt >= 0.0 && now - g.LastSampleAt < 1.0 / 120.0) return;
    const double dt = g.LastSampleAt >= 0.0 ? now - g.LastSampleAt : 1.0 / 60.0;
    g.LastSampleAt = now;
    // Pressure: the slider's, eased down as the pointer speeds up (a fast pass is a light one).
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float speed = std::sqrt((m.x - g.LastMouse.x) * (m.x - g.LastMouse.x) + (m.y - g.LastMouse.y) * (m.y - g.LastMouse.y)) /
                        (float)std::max(dt, 1e-3);
    g.LastMouse = m;
    const float pen = PenPressure(g);
    const float target = pen >= 0.0f ? pen : g.VelocityPressure ? g.Pressure * std::clamp(1.15f - speed / 2200.0f, 0.35f, 1.0f) : g.Pressure;
    g.LivePressure += (target - g.LivePressure) * 0.35f;
    g.Live.Samples.push_back(Graffiti::SpraySample::Make(uv, g.LivePressure, g.DistanceCm * 0.01f, (float)t));
    const size_t before = g.Pending.size();
    g.Builder.Add(g.Live.Samples.back(), g.Pending);
    for (size_t i = before; i < g.Pending.size(); ++i) g.PendingSeq.push_back(g.NextSeq++);
    DrainDue(g, g.Live.Samples.back().Time(), false);
    g.Comp.LiveComposite(*g.Piece, g.Program);
    g.LastUv = uv;
    EditorSound::SetLoop(g.Hiss, 0.4f + 0.6f * g.LivePressure, 0.88f + 0.24f * g.LivePressure);
}

void EndStroke(GraffitiDesigner& g) {
    if (!g.Stroking) return;
    g.Stroking = false;
    DrainDue(g, 0.0, true);
    g.Comp.LiveComposite(*g.Piece, g.Program);
    g.Comp.EndLive();
    EditorSound::StopLoop(g.Hiss);
    g.Hiss = -1;
    EditorSound::Play(EditorSound::Event::SprayEnd);
    if (g.Live.Samples.size() >= 2 && g.StrokeLayer >= 0 && g.StrokeLayer < (int)g.Piece->Layers.size()) {
        g.Piece->Layers[(size_t)g.StrokeLayer].Strokes.push_back(std::move(g.Live));
        SaveNow(g); // one stroke, one undo step (the file journal)
        g.NeedsBuild = true;
    }
    g.Live = Graffiti::Stroke{};
    if (g.BakeAgain) {
        g.BakeAgain = false;
        StartBake(g);
    }
}

// `path` by value: the reload passes g.Piece->Path, which dies with the old piece when g.Piece is replaced.
void LoadInto(GraffitiDesigner& g, const std::string path) {
    auto p = Graffiti::Piece::Load(path);
    if (!p || p->Missing) {
        g.Status = "Couldn't open " + fs::u8path(path).filename().u8string();
        return;
    }
    const bool samePiece = g.Piece && AssetDatabase::PathKey(g.Piece->Path) == AssetDatabase::PathKey(path);
    g.Piece = p;
    g.Stamp = FileStamp(path);
    if (!samePiece) {
        g.LayerIds.clear();
        g.PrevRow.clear();
        g.Target = entt::null;
        g.Selected = p->Layers.empty() ? -1 : (int)p->Layers.size() - 1;
    }
    g.Selected = std::min(g.Selected, (int)p->Layers.size() - 1);
    SyncLayerIds(g);
    g.NeedsBuild = true;
    g.Dirty = false;
    g.Status.clear();
}

// Canvas-uv (y down) <-> the decal box: x right, y up, +-0.5 across the box.
glm::vec2 BoxToUv(const glm::vec3& local) { return {local.x + 0.5f, 0.5f - local.y}; }

// --- The stamp picker -------------------------------------------------------------------------------

constexpr const char* kStampRoot = "assets/Decals/Graffiti";
constexpr size_t kMaxRecent = 12;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string BaseName(const std::string& rel) {
    const size_t at = rel.find_last_of('/');
    return at == std::string::npos ? rel : rel.substr(at + 1);
}
// "T_Decal_GraffitiShape_001.png" -> "Shape_001"
std::string ShortStampName(const std::string& rel) {
    std::string n = BaseName(rel);
    if (const size_t dot = n.rfind('.'); dot != std::string::npos) n.erase(dot);
    for (const char* prefix : {"T_Decal_Graffiti", "T_Decal_"})
        if (n.rfind(prefix, 0) == 0) { n.erase(0, std::strlen(prefix)); break; }
    return n;
}

// Every subfolder of Decals/Graffiti that holds at least one PNG.
void ScanStamps(StampPicker& pk) {
    pk.Folders.clear();
    std::error_code ec;
    const fs::path root = fs::u8path(ProjectPaths::Resolve(kStampRoot));
    for (const auto& d : fs::directory_iterator(root, ec)) {
        if (!d.is_directory(ec)) continue;
        StampPicker::Folder f;
        f.Name = d.path().filename().u8string();
        for (const auto& e : fs::directory_iterator(d.path(), ec)) {
            if (!e.is_regular_file(ec) || LowerAscii(e.path().extension().u8string()) != ".png") continue;
            f.Files.push_back(std::string(kStampRoot) + "/" + f.Name + "/" + e.path().filename().u8string());
        }
        if (f.Files.empty()) continue;
        std::sort(f.Files.begin(), f.Files.end());
        pk.Folders.push_back(std::move(f));
    }
    std::sort(pk.Folders.begin(), pk.Folders.end(), [](const auto& a, const auto& b) { return a.Name < b.Name; });
}

// A square on a dark checker holding the stamp's thumbnail (or a placeholder while it loads).
void DrawStampThumb(GraffitiDesigner& g, AssetThumbnailLoader& thumbs, const std::string& rel, ImVec2 p0, float side, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + side, p0.y + side);
    dl->AddRectFilled(p0, p1, IM_COL32(26, 26, 26, 255), 3.0f * scale);
    const float sq = std::max(4.0f, std::floor(side / 9.0f));
    const ImU32 light = IM_COL32(44, 44, 44, 255);
    dl->PushClipRect(p0, p1, true);
    for (int y = 0; y * sq < side; ++y)
        for (int x = (y & 1); x * sq < side; x += 2)
            dl->AddRectFilled(ImVec2(p0.x + x * sq, p0.y + y * sq), ImVec2(p0.x + (x + 1) * sq, p0.y + (y + 1) * sq), light);
    dl->PopClipRect();
    const std::string abs = Abs(rel);
    const AssetThumbnailLoader::Thumb t = thumbs.Get(abs, AssetThumbnailLoader::Source::Image);
    if (std::find(g.Picker.Requested.begin(), g.Picker.Requested.end(), abs) == g.Picker.Requested.end())
        g.Picker.Requested.push_back(abs);
    if (t.Tex) {
        dl->AddImage((ImTextureID)(intptr_t)t.Tex, p0, p1, ImVec2(0.0f, t.FlipV ? 1.0f : 0.0f), ImVec2(1.0f, t.FlipV ? 0.0f : 1.0f));
    } else {
        const char* dots = "...";
        const ImVec2 ts = ImGui::CalcTextSize(dots);
        dl->AddText(ImVec2(p0.x + (side - ts.x) * 0.5f, p0.y + (side - ts.y) * 0.5f), EditorTheme::U32(EditorTheme::Dim), dots);
    }
}

// The popup. Returns the picked project-relative path, or "" when nothing was picked this frame.
std::string DrawStampPickerPopup(GraffitiDesigner& g, AssetThumbnailLoader& thumbs, float scale, const std::string& current) {
    StampPicker& pk = g.Picker;
    std::string picked;
    if (pk.WantOpen) {
        pk.WantOpen = false;
        ScanStamps(pk);
        pk.Search[0] = 0;
        pk.Tab = pk.Recent.empty() ? 1 : 0;
        if (pk.Tab == 1 && pk.Folders.empty()) pk.Tab = 0;
        ImGui::OpenPopup("##stampPicker");
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 460.0f * scale), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##stampPicker")) return picked;

    // Folder tabs: Recent, then one per non-empty folder.
    std::vector<std::string> names{"Recent"};
    for (const auto& f : pk.Folders) names.push_back(f.Name);
    std::vector<const char*> labels;
    for (const auto& n : names) labels.push_back(n.c_str());
    pk.Tab = std::clamp(pk.Tab, 0, (int)labels.size() - 1);
    EditorUIPrimitives::Segmented("##stampTabs", &pk.Tab, labels.data(), (int)labels.size());

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##stampSearch", "Search by filename", pk.Search, sizeof(pk.Search));

    // The files for this tab, filtered by the search text.
    const std::vector<std::string>& src = pk.Tab == 0 ? pk.Recent : pk.Folders[(size_t)pk.Tab - 1].Files;
    const std::string needle = LowerAscii(pk.Search);
    std::vector<const std::string*> shown;
    for (const std::string& f : src)
        if (needle.empty() || LowerAscii(BaseName(f)).find(needle) != std::string::npos) shown.push_back(&f);

    const float cell = 72.0f * scale, gap = 6.0f * scale;
    ImGui::BeginChild("##stampGrid", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_None);
    const float width = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, (int)((width + gap) / (cell + gap)));
    const int rows = ((int)shown.size() + cols - 1) / cols;
    if (shown.empty()) ImGui::TextDisabled("%s", pk.Tab == 0 && needle.empty() ? "Nothing picked yet this session." : "No images match.");
    ImGuiListClipper clip;
    clip.Begin(rows, cell + gap);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const ImVec2 rowTop = ImGui::GetCursorScreenPos();
            for (int c = 0; c < cols; ++c) {
                const int i = r * cols + c;
                if (i >= (int)shown.size()) break;
                const std::string& rel = *shown[(size_t)i];
                const ImVec2 p0(rowTop.x + c * (cell + gap), rowTop.y);
                ImGui::SetCursorScreenPos(p0);
                ImGui::PushID(i);
                ImGui::InvisibleButton("##cell", ImVec2(cell, cell));
                EditorTestTag(("stamp:" + BaseName(rel)).c_str());
                const bool hovered = ImGui::IsItemHovered();
                const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                ImGui::PopID();
                DrawStampThumb(g, thumbs, rel, p0, cell, scale);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p1(p0.x + cell, p0.y + cell);
                if (AssetDatabase::PathKey(rel) == AssetDatabase::PathKey(current))
                    dl->AddRect(p0, p1, EditorTheme::U32(EditorTheme::Accent), 3.0f * scale, 0, 2.0f);
                else
                    dl->AddRect(p0, p1, EditorTheme::U32(hovered ? EditorTheme::Strong : EditorTheme::Hairline), 3.0f * scale);
                if (hovered) EditorUI::SetTooltip("%s", ShortStampName(rel).c_str());
                if (clicked) picked = rel;
            }
            ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowTop.y + cell + gap));
            ImGui::Dummy(ImVec2(0, 0));
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%d image%s   Esc closes", (int)shown.size(), shown.size() == 1 ? "" : "s");

    if (!picked.empty()) {
        auto& R = pk.Recent;
        R.erase(std::remove(R.begin(), R.end(), picked), R.end());
        R.insert(R.begin(), picked);
        if (R.size() > kMaxRecent) R.resize(kMaxRecent);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return picked;
}

// A thumbnail button for the current stamp (the Stamp tool's options); true when pressed.
bool StampThumbButton(GraffitiDesigner& g, AssetThumbnailLoader& thumbs, const std::string& rel, float side, float scale) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##stampThumb", ImVec2(side, side));
    EditorTestTag("stamp-tool-thumb");
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    DrawStampThumb(g, thumbs, rel, p0, side, scale);
    ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x + side, p0.y + side),
                                        EditorTheme::U32(hovered ? EditorTheme::Accent : EditorTheme::Strong), 3.0f * scale);
    if (hovered) EditorUI::SetTooltip("Stamp: %s\nClick to choose another", ShortStampName(rel).c_str());
    return clicked;
}

} // namespace

// =====================================================================================================
// EditorLayer
// =====================================================================================================

void EditorLayer::OpenGraffitiPiece(const std::string& path) {
    if (!m_Graffiti) m_Graffiti = std::make_shared<GraffitiDesigner>();
    GraffitiDesigner& g = *m_Graffiti;
    if (g.Stroking) EndStroke(g);
    if (g.Dirty) SaveNow(g);
    LoadInto(g, Abs(path));
    m_ShowGraffitiDesigner = true;
    ImGui::SetWindowFocus(EditorPanels::GraffitiDesigner);
    EditorSound::Play(EditorSound::Event::AssetOpen);
}

std::string EditorLayer::CreateGraffitiPiece(const std::string& folder, const std::string& stem) {
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    fs::path target = fs::u8path(folder) / ("GP_" + stem + ".graffiti");
    for (int n = 1; fs::exists(target, ec); ++n) target = fs::u8path(folder) / ("GP_" + stem + "_" + std::to_string(n) + ".graffiti");
    const auto piece = NewPiece(target.u8string());
    if (!piece->Save()) {
        Log::Error("Graffiti: couldn't write '" + target.u8string() + "'.");
        return {};
    }
    return target.u8string();
}

bool EditorLayer::GraffitiViewportSprayActive() const {
    return m_Graffiti && m_ShowGraffitiDesigner && m_Graffiti->ViewportSpray;
}

void EditorLayer::GraffitiFrame(World& world) {
    if (!m_Graffiti) return;
    GraffitiDesigner& g = *m_Graffiti;
    // A bake's maps arriving from the GPU, then its files from the worker.
    if (g.BakeStage == 1) {
        Graffiti::BakeImages img;
        if (g.Comp.PollBake(img)) {
            auto piece = std::make_shared<Graffiti::Piece>(*g.Piece);
            // The worker writes the files after this edit window closes, so the pre-images are taken
            // here and the bake becomes its undo step when the worker is done (below).
            g.BakeBefore.clear();
            if (g.BakeUndoable)
                for (const char* map : {"Albedo", "Normal", "Surface", "Aux"})
                g.BakeBefore.push_back(EditorFileHistory::Capture(fs::u8path(piece->MapPath(map))));
                g.BakeBefore.push_back(EditorFileHistory::Capture(fs::u8path(piece->DecalPath())));
            g.Encoding = std::async(std::launch::async, [piece, img = std::move(img)]() -> uint64_t {
                const Graffiti::BakeFiles files = Graffiti::EncodePngs(img);
                return Graffiti::WriteBake(*piece, files) ? HashFiles(files) : 0ull;
            });
            g.BakeStage = 2;
            g.NeedsBuild = true; // the bake render replaced the canvas; show the live program again
        }
    } else if (g.BakeStage == 2 && g.Encoding.valid() &&
               g.Encoding.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const uint64_t h = g.Encoding.get();
        g.BakeStage = 0;
        if (h) {
            g.LastBakeHash = h;
            ++g.Bakes;
            g.BakeDoneAt = Now();
            g.Status = "Baked " + fs::u8path(g.Piece->DecalPath()).filename().u8string();
            if (g.BakeUndoable) JournalFilesBefore(world, g.BakeBefore, "Bake Graffiti Piece");
            g.BakeBefore.clear();
            if (DecalSystem* ds = DecalSystem::Current()) ds->Refresh(g.Piece->DecalPath());
            EditorSound::Play(EditorSound::Event::GraffitiBakeDone);
        } else {
            g.Status = "Bake failed (see the Console)";
        }
        if (g.BakeAgain && !g.Stroking) {
            g.BakeAgain = false;
            const bool again = g.BakeAgainUndoable;
            g.BakeAgainUndoable = false;
            StartBake(g, again);
        }
    }
    if (!g.Piece) return;
    // Undo / redo (or anything else) rewrote the recipe: reload it, and re-bake a piece on a wall.
    if (!g.Stroking && Now() - g.LastStampCheck > 0.25) {
        g.LastStampCheck = Now();
        const long long stamp = FileStamp(g.Piece->Path);
        if (stamp != g.Stamp && stamp != 0) {
            LoadInto(g, g.Piece->Path);
            if (g.Target != entt::null && world.Registry.valid(g.Target)) StartBake(g);
        }
    }
    if (g.Target != entt::null && !world.Registry.valid(g.Target)) g.Target = entt::null;
    // Recipe edits are saved once nothing is held (a drag or a typing session is one undo step).
    if (g.Dirty && !g.Stroking && !ImGui::IsAnyItemActive() && !g.Moving) SaveNow(g);
}

// --- Scene-view spraying ---------------------------------------------------------------------------------

bool EditorLayer::HandleGraffitiViewportSpray(World& world, Camera& editorCamera, bool leftPressed, bool leftDown) {
    if (!GraffitiViewportSprayActive()) return false;
    GraffitiDesigner& g = *m_Graffiti;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return false;

    // The cursor ray.
    const glm::mat4 vp = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();
    const glm::mat4 invVP = glm::inverse(vp);
    const ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = (2.0f * (mouse.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    const float ndcY = 1.0f - (2.0f * (mouse.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f), farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    const glm::vec3 origin(nearP), dir = glm::normalize(glm::vec3(farP) - origin);

    auto boxToWorld = [&](entt::entity e) {
        const auto& d = world.Registry.get<DecalComponent>(e);
        return world.ComposeWorldTransform(e) * glm::scale(glm::mat4(1.0f), d.Size);
    };
    // Where the ray crosses the target decal's plane (box z = 0), as canvas uv.
    auto uvOnTarget = [&](glm::vec2& uv) {
        if (g.Target == entt::null || !world.Registry.valid(g.Target) || !world.Registry.all_of<DecalComponent>(g.Target)) return false;
        const glm::mat4 inv = glm::inverse(boxToWorld(g.Target));
        const glm::vec3 o(inv * glm::vec4(origin, 1.0f)), d(inv * glm::vec4(dir, 0.0f));
        if (std::abs(d.z) < 1e-6f) return false;
        const float t = -o.z / d.z;
        if (t < 0.0f) return false;
        uv = BoxToUv(o + d * t);
        return true;
    };

    if (leftPressed) {
        const DecalSpot spot = DecalSpotUnderMouse(world, editorCamera);
        const std::string decalKey = g.Piece ? AssetDatabase::PathKey(g.Piece->DecalPath()) : std::string();
        bool onTarget = false;
        if (spot.Decal != entt::null) {
            const auto& d = world.Registry.get<DecalComponent>(spot.Decal);
            if (!decalKey.empty() && AssetDatabase::PathKey(Abs(d.AssetPath)) == decalKey) {
                g.Target = spot.Decal;
                onTarget = true;
            }
        }
        if (!onTarget) {
            if (!spot.OnSurface) {
                EditorSound::Play(EditorSound::Event::Blocked);
                return true;
            }
            // The open piece goes on this wall when it isn't on one yet; otherwise a fresh piece starts
            // here. Either way: a first bake for the .decal, then the decal itself.
            if (g.Stroking) EndStroke(g);
            if (g.Dirty) SaveNow(g);
            bool placed = false;
            if (g.Piece)
                for (auto [e, d] : world.Registry.view<const DecalComponent>().each())
                    placed |= AssetDatabase::PathKey(Abs(d.AssetPath)) == decalKey;
            if (!g.Piece || placed) {
                const std::string path = CreateGraffitiPiece(ProjectPaths::Resolve(kPiecesFolder), "Spray");
                if (path.empty()) return true;
                LoadInto(g, path);
            }
            const std::string path = g.Piece->Path;
            Rebuild(g);
            if (!BakeNow(g)) {
                g.Status = "Couldn't bake the new piece";
                return true;
            }
            g.Target = PlaceDecal(world, g.Piece->DecalPath(), spot.Point, spot.Normal, editorCamera.Right());
            g.Status = "Spraying onto " + fs::u8path(path).filename().u8string();
        }
        glm::vec2 uv;
        if (uvOnTarget(uv)) BeginStroke(g, uv, g.Active == GTool::Erase, /*inViewport=*/true);
        return true;
    }
    if (g.Stroking && g.StrokeInViewport) {
        glm::vec2 uv;
        if (leftDown) {
            if (uvOnTarget(uv)) StrokeSample(g, uv);
        } else {
            EndStroke(g);
            g.OverlayUntil = Now() + 4.0;
            StartBake(g);
        }
        return true;
    }
    return true; // spraying owns the viewport's left button
}

void EditorLayer::DrawGraffitiViewportOverlay(World& world, Camera& editorCamera) {
    if (!m_Graffiti) return;
    GraffitiDesigner& g = *m_Graffiti;
    if (g.Target == entt::null || !world.Registry.valid(g.Target) || !world.Registry.all_of<DecalComponent>(g.Target)) return;
    // While a stroke is live, and after it until the re-baked decal draws, the canvas is drawn over the wall.
    bool show = g.Stroking && g.StrokeInViewport;
    if (!show && Now() < g.OverlayUntil) {
        DecalSystem* ds = DecalSystem::Current();
        const bool resident = g.BakeStage == 0 && ds && g.Piece && ds->Resident(g.Piece->DecalPath());
        show = !resident;
        if (resident) g.OverlayUntil = -1.0;
    }
    if (!show || !g.Comp.OverlayTexture() || m_ViewportSize.x <= 0.0f) {
        g.OccVisible.clear(); // the next time it shows, the cells are worked out afresh
        return;
    }
    const auto& d = world.Registry.get<DecalComponent>(g.Target);
    const glm::mat4 box = world.ComposeWorldTransform(g.Target) * glm::scale(glm::mat4(1.0f), d.Size);
    const glm::mat4 vp = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();
    auto project = [&](float u, float v, ImVec2& out) {
        const glm::vec4 c = vp * box * glm::vec4(u - 0.5f, 0.5f - v, 0.0f, 1.0f); // canvas uv -> box
        if (c.w <= 1e-4f) return false;
        out = ImVec2(m_ViewportPos.x + (c.x / c.w * 0.5f + 0.5f) * m_ViewportSize.x,
                     m_ViewportPos.y + (0.5f - c.y / c.w * 0.5f) * m_ViewportSize.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                     ImVec2(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y), true);
    // A grid, so the affine quads stay close to the true perspective - and so geometry standing in
    // front of the wall can hide the cells behind it, as the bake's depth test does: each cell's
    // centre is ray-cast from the eye; a surface nearer than the wall point means the cell is covered.
    // The result is cached while the camera, the decal and (for 0.3 s) the scene hold still.
    constexpr int kGrid = 24;
    const glm::mat4 key = vp * box;
    if (g.OccVisible.size() != (size_t)(kGrid * kGrid) || std::memcmp(&key, &g.OccKey, sizeof(key)) != 0 || Now() - g.OccAt > 0.3) {
        g.OccKey = key;
        g.OccAt = Now();
        g.OccVisible.assign((size_t)(kGrid * kGrid), 1);
        const glm::vec3 eye = editorCamera.Position;
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                const float u = ((float)x + 0.5f) / kGrid, v = ((float)y + 0.5f) / kGrid;
                const glm::vec3 wall(box * glm::vec4(u - 0.5f, 0.5f - v, 0.0f, 1.0f));
                const float dist = glm::length(wall - eye);
                if (dist < 0.05f) continue;
                glm::vec3 hitPoint, hitNormal;
                if (CastDecalRay(world, eye, (wall - eye) / dist, dist - 0.05f, hitPoint, hitNormal))
                    g.OccVisible[(size_t)(y * kGrid + x)] = 0;
            }
    }
    const ImTextureID tex = (ImTextureID)(intptr_t)g.Comp.OverlayTexture();
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x) {
            if (!g.OccVisible[(size_t)(y * kGrid + x)]) continue;
            const float u0 = (float)x / kGrid, u1 = (float)(x + 1) / kGrid, v0 = (float)y / kGrid, v1 = (float)(y + 1) / kGrid;
            ImVec2 p[4];
            if (!project(u0, v0, p[0]) || !project(u1, v0, p[1]) || !project(u1, v1, p[2]) || !project(u0, v1, p[3])) continue;
            // The texture's top row is v = 1 (GL rows run up).
            dl->AddImageQuad(tex, p[0], p[1], p[2], p[3], ImVec2(u0, 1.0f - v0), ImVec2(u1, 1.0f - v0), ImVec2(u1, 1.0f - v1),
                             ImVec2(u0, 1.0f - v1));
        }
    dl->PopClipRect();
}

// --- The panel -----------------------------------------------------------------------------------------

void EditorLayer::DrawGraffitiDesignerPanel(World& world) {
    if (!m_ShowGraffitiDesigner) {
        if (m_Graffiti && m_Graffiti->Stroking) EndStroke(*m_Graffiti);
        if (m_Graffiti) { // free the stamp picker's thumbnails
            for (const std::string& p : m_Graffiti->Picker.Requested) m_AssetThumbs.Invalidate(p);
            m_Graffiti->Picker.Requested.clear();
        }
        return;
    }
    if (!m_Graffiti) m_Graffiti = std::make_shared<GraffitiDesigner>();
    GraffitiDesigner& g = *m_Graffiti;
    ImGui::SetNextWindowSize(ImVec2(1100, 680), ImGuiCond_FirstUseEver);
    const bool open = ImGui::Begin(EditorPanels::GraffitiDesigner, &m_ShowGraffitiDesigner);
    if (!open) {
        ImGui::End();
        return;
    }
    auto& M = Glass::Motion();
    const ImGuiIO& io = ImGui::GetIO();

    // ---- Top bar: file, bake, apply, Scene-view spray ----
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_PLUS "  New")) {
        const std::string path = CreateGraffitiPiece(ProjectPaths::Resolve(kPiecesFolder), "Piece");
        if (!path.empty()) OpenGraffitiPiece(path);
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("A new blank piece (GP_*.graffiti) in %s.", kPiecesFolder);
    ImGui::SameLine();
    {
        const std::string current = g.Piece ? fs::u8path(g.Piece->Path).filename().u8string() : std::string("(no piece)");
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##piece", current.c_str())) {
            std::error_code ec;
            const fs::path dir = fs::u8path(ProjectPaths::Resolve(kPiecesFolder));
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == ".graffiti") files.push_back(it->path());
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files)
                if (ImGui::Selectable(f.filename().u8string().c_str(), g.Piece && fs::u8path(g.Piece->Path) == f))
                    OpenGraffitiPiece(f.u8string());
            if (files.empty()) ImGui::TextDisabled("No pieces yet");
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g.Piece);
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_FLOPPY_DISK "  Save")) SaveNow(g);
    ImGui::SameLine();
    if (EditorUIPrimitives::PrimaryButton(ICON_FA_FIRE "  Bake")) StartBake(g, true);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Bake the layers into %s (albedo, normal, surface and glow maps) and its .decal.",
                             g.Piece ? ("DA_Piece_" + g.Piece->Name + ".decal").c_str() : "the piece's decal");
    ImGui::SameLine();
    const bool decalSelected = m_Selected != entt::null && world.Registry.valid(m_Selected) && world.Registry.all_of<DecalComponent>(m_Selected);
    ImGui::BeginDisabled(!decalSelected);
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_CHECK "  Apply to selected decal")) {
        if (!fs::exists(fs::u8path(g.Piece->DecalPath()))) BakeNow(g);
        PushUndo(world, "Apply Graffiti Piece");
        auto& dc = world.Registry.get<DecalComponent>(m_Selected);
        dc.AssetPath = g.Piece->DecalPath();
        dc.Size = glm::vec3(g.Piece->Metres, dc.Size.z);
        g.Target = m_Selected;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        if (EditorUIPrimitives::ActionButton(ICON_FA_SPRAY_CAN "  Spray in Scene view", "##viewportSpray", nullptr,
                                             g.ViewportSpray)) { // ActionButton plays the click / toggle-off sound
            g.ViewportSpray = !g.ViewportSpray;
            if (g.ViewportSpray && g.Active != GTool::Erase) g.Active = GTool::Spray;
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Aim at a wall in the Scene view and hold the left button to spray. Spraying off this "
                                 "piece's decal starts a fresh piece with its own decal there.");
    }
    ImGui::EndDisabled();
    if (!g.Status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g.Status.c_str());
    }

    if (!g.Piece) {
        ImGui::Spacing();
        ImGui::TextDisabled("Open a piece above or press New. With a piece open, Spray in Scene view puts it on the wall you spray.");
        ImGui::End();
        return;
    }
    SyncLayerIds(g);
    auto& L = g.Piece->Layers;
    if (g.Selected >= (int)L.size()) g.Selected = (int)L.size() - 1;

    // ---- Left column: tools, brush, colour, layers, layer and piece settings ----
    const float sideW = 300.0f * m_UIScale;
    ImGui::BeginChild("##gside", ImVec2(sideW, 0), ImGuiChildFlags_Borders);
    for (int t = 0; t < (int)GTool::Count; ++t) {
        if (t) ImGui::SameLine();
        if (EditorUIPrimitives::ActionButton(ToolIcon((GTool)t), ToolName((GTool)t), nullptr, (int)g.Active == t,
                                             ImVec2(36.0f * m_UIScale, 0)))
            g.Active = (GTool)t;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", ToolName((GTool)t));
    }

    if (g.Active == GTool::Stamp) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Stamp");
        ImGui::SameLine();
        if (StampThumbButton(g, m_AssetThumbs, g.StampTexture, 40.0f * m_UIScale, m_UIScale)) {
            g.Picker.WantOpen = true;
            g.Picker.Target = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ShortStampName(g.StampTexture).c_str());
    }

    if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* caps[] = {"Fat", "Skinny", "Calligraphy"};
        int cap = (int)g.Cap;
        if (EditorUIPrimitives::Segmented("##cap", &cap, caps, 3)) g.Cap = (Graffiti::Cap)cap;
        // Odometer readouts: pressure (live while spraying) and the can's distance.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::TextDisabled("Pressure");
        ImGui::SameLine(90.0f * m_UIScale);
        at = ImGui::GetCursorScreenPos();
        float w = M.Odometer(ImGui::GetID("##odoPressure"), dl, at, (double)((g.Stroking ? g.LivePressure : g.Pressure) * 100.0f), 0, col, "%");
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
        ImGui::SameLine(160.0f * m_UIScale);
        ImGui::TextDisabled("Distance");
        ImGui::SameLine();
        at = ImGui::GetCursorScreenPos();
        w = M.Odometer(ImGui::GetID("##odoDistance"), dl, at, (double)g.DistanceCm, 0, col, " cm");
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##pressure", &g.Pressure, 0.05f, 1.0f, "Pressure %.2f");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##distance", &g.DistanceCm, 2.0f, 60.0f, "Distance %.0f cm");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##flow", &g.Flow, 0.1f, 2.0f, "Flow %.2f");
        if (g.Cap == Graffiti::Cap::Calligraphy) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##nib", &g.NibAngle, -90.0f, 90.0f, "Nib angle %.0f deg");
        }
        ImGui::Checkbox("Pressure from pointer speed", &g.VelocityPressure);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("A fast pass sprays lighter, a slow one heavier. A pen's own pressure replaces this.");
        {
            const float pen = PenInput::Pressure();
            if (pen >= 0.0f) ImGui::TextDisabled("Pen pressure: %.0f%%", pen * 100.0f);
            else ImGui::TextDisabled("Pen pressure: none (mouse)");
        }
    }

    if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit3("##colour", &g.Color.x, ImGuiColorEditFlags_DisplayHex);
        static const glm::vec3 kSwatches[] = {{0.95f, 0.92f, 0.85f}, {0.06f, 0.06f, 0.06f}, {0.9f, 0.16f, 0.12f}, {0.98f, 0.55f, 0.1f},
                                              {0.96f, 0.86f, 0.18f}, {0.25f, 0.75f, 0.3f},  {0.15f, 0.55f, 0.95f}, {0.55f, 0.25f, 0.85f},
                                              {0.95f, 0.35f, 0.7f},  {0.75f, 0.75f, 0.78f}};
        auto swatch = [&](const char* id, const glm::vec3& c, int i) {
            if (i % 10) ImGui::SameLine();
            if (ImGui::ColorButton(id, ImVec4(c.x, c.y, c.z, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20) * m_UIScale)) {
                g.Color = c;
                EditorSound::Play(EditorSound::Event::Click);
            }
        };
        for (int i = 0; i < 10; ++i) {
            ImGui::PushID(i);
            swatch("##sw", kSwatches[i], i);
            ImGui::PopID();
        }
        if (!g.Recent.empty()) {
            ImGui::TextDisabled("Recent");
            for (int i = 0; i < (int)g.Recent.size(); ++i) {
                ImGui::PushID(100 + i);
                swatch("##rc", g.Recent[(size_t)i], i);
                ImGui::PopID();
            }
        }
    }

    if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto addLayer = [&](Graffiti::LayerType type) {
            Graffiti::Layer l;
            l.Type = type;
            l.Name = std::string(Graffiti::LayerTypeName(type)) + " " + std::to_string(L.size() + 1);
            l.Name[0] = (char)std::toupper((unsigned char)l.Name[0]);
            l.Color = Graffiti::Quantise(g.Color);
            if (type == Graffiti::LayerType::Text) {
                l.Text = "TAG";
                l.Outline = true;
                g.CascadeLayer = -1;
            }
            if (type == Graffiti::LayerType::Stamp) {
                l.Texture = g.StampTexture;
                l.Luma = false; // the cooked graffiti packs carry their shape in alpha
            }
            if (type == Graffiti::LayerType::Fill) l.Opacity = 0.85f;
            const int at = g.Selected >= 0 ? g.Selected + 1 : (int)L.size();
            L.insert(L.begin() + at, l);
            g.LayerIds.insert(g.LayerIds.begin() + at, g.NextLayerId++);
            g.Selected = at;
            if (type == Graffiti::LayerType::Text) {
                g.CascadeLayer = at;
                g.CascadeFrom = 0;
                g.CascadeStart = Now();
            }
            EditorSound::Play(EditorSound::Event::GraffitiLayerAdd);
            Edited(g);
        };
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_PLUS " Add")) ImGui::OpenPopup("##addLayer");
        if (ImGui::BeginPopup("##addLayer")) {
            if (ImGui::MenuItem(ICON_FA_SPRAY_CAN "  Spray")) addLayer(Graffiti::LayerType::Spray);
            if (ImGui::MenuItem(ICON_FA_FONT "  Text")) addLayer(Graffiti::LayerType::Text);
            if (ImGui::MenuItem(ICON_FA_STAMP "  Stamp")) addLayer(Graffiti::LayerType::Stamp);
            if (ImGui::MenuItem(ICON_FA_FILL_DRIP "  Fill / gradient")) addLayer(Graffiti::LayerType::Fill);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(g.Selected < 0);
        if (EditorUIPrimitives::DangerIconButton(ICON_FA_TRASH, "##removeLayer", nullptr) && g.Selected >= 0) {
            L.erase(L.begin() + g.Selected);
            g.LayerIds.erase(g.LayerIds.begin() + g.Selected);
            g.Selected = std::min(g.Selected, (int)L.size() - 1);
            EditorSound::Play(EditorSound::Event::GraffitiLayerRemove);
            Edited(g);
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Remove the selected layer");
        ImGui::EndDisabled();

        // Top layer first. Rows glide to their new place when the order changes.
        int moveFrom = -1, moveTo = -1;
        const float rowH = kRowH * m_UIScale;
        const ImVec2 listStart = ImGui::GetCursorScreenPos();
        const float listW = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(listW, rowH * (float)L.size()));
        for (int row = 0; row < (int)L.size(); ++row) {
            const int i = (int)L.size() - 1 - row;
            Graffiti::Layer& l = L[(size_t)i];
            const uint32_t lid = g.LayerIds[(size_t)i];
            const auto prevIt = g.PrevRow.find(lid);
            const float prev = prevIt == g.PrevRow.end() ? (float)row : prevIt->second;
            const float shown = M.Follow((ImGuiID)(0x6A11F000u ^ lid), (float)row, prev, Glass::SelectionGlide);
            g.PrevRow[lid] = (float)row;
            ImGui::SetCursorScreenPos(ImVec2(listStart.x, listStart.y + shown * rowH));
            ImGui::PushID((int)lid);
            if (EditorUIPrimitives::ActionButton(l.Visible ? ICON_FA_EYE : ICON_FA_EYE_SLASH, "##visible", nullptr)) {
                l.Visible = !l.Visible;
                Edited(g);
            }
            ImGui::SameLine();
            const std::string label = std::string(LayerIcon(l.Type)) + "  " + l.Name;
            if (ImGui::Selectable(label.c_str(), g.Selected == i, 0, ImVec2(listW - 60.0f * m_UIScale, 0))) g.Selected = i;
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("GRAFFITI_LAYER", &i, sizeof(int));
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GRAFFITI_LAYER")) {
                    moveFrom = *(const int*)pl->Data;
                    moveTo = i;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (EditorUIPrimitives::ActionButton(ICON_FA_CHEVRON_UP, "##up", nullptr) && i + 1 < (int)L.size()) { moveFrom = i; moveTo = i + 1; }
            ImGui::SameLine();
            if (EditorUIPrimitives::ActionButton(ICON_FA_CHEVRON_DOWN, "##down", nullptr) && i > 0) { moveFrom = i; moveTo = i - 1; }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(listStart.x, listStart.y + rowH * (float)L.size()));
        ImGui::Dummy(ImVec2(1, 1));
        if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveFrom < (int)L.size()) {
            Graffiti::Layer moved = std::move(L[(size_t)moveFrom]);
            const uint32_t id = g.LayerIds[(size_t)moveFrom];
            L.erase(L.begin() + moveFrom);
            g.LayerIds.erase(g.LayerIds.begin() + moveFrom);
            L.insert(L.begin() + moveTo, std::move(moved));
            g.LayerIds.insert(g.LayerIds.begin() + moveTo, id);
            g.Selected = moveTo;
            EditorSound::Play(EditorSound::Event::GraffitiLayerReorder);
            Edited(g);
        }
    }

    if (g.Selected >= 0 && g.Selected < (int)L.size() && ImGui::CollapsingHeader("Layer", ImGuiTreeNodeFlags_DefaultOpen)) {
        Graffiti::Layer& l = L[(size_t)g.Selected];
        bool ch = false;
        char name[64];
        std::snprintf(name, sizeof(name), "%s", l.Name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##lname", name, sizeof(name))) { l.Name = name; ch = true; }
        static const char* blends[] = {"Normal", "Multiply", "Screen", "Add", "Erase"};
        int blend = (int)l.Mode;
        ImGui::SetNextItemWidth(120.0f * m_UIScale);
        if (ImGui::Combo("Blend", &blend, blends, 5)) { l.Mode = (Graffiti::Blend)blend; ch = true; }
        ch |= ImGui::SliderFloat("Opacity", &l.Opacity, 0.0f, 1.0f);
        ch |= ImGui::Checkbox("Clip to layers below", &l.Clip);
        if (l.Type != Graffiti::LayerType::Spray) ch |= ImGui::ColorEdit3("Colour", &l.Color.x, ImGuiColorEditFlags_NoInputs);
        if (l.Type != Graffiti::LayerType::Spray) {
            ch |= ImGui::Checkbox("Gradient", &l.Gradient);
            if (l.Gradient) {
                ImGui::SameLine();
                ch |= ImGui::ColorEdit3("##gto", &l.GradientTo.x, ImGuiColorEditFlags_NoInputs);
                ch |= ImGui::SliderFloat("Angle", &l.GradientAngle, 0.0f, 360.0f, "%.0f deg");
            }
        }
        ch |= ImGui::Checkbox("Outline", &l.Outline);
        if (l.Outline) {
            ImGui::SameLine();
            ch |= ImGui::ColorEdit3("##oc", &l.OutlineColor.x, ImGuiColorEditFlags_NoInputs);
            ch |= ImGui::SliderFloat("Width", &l.OutlineWidth, 0.002f, 0.05f, "%.3f");
        }
        ch |= ImGui::Checkbox("Drop shadow", &l.Shadow);
        if (l.Shadow) {
            ImGui::SameLine();
            ch |= ImGui::ColorEdit3("##sc", &l.ShadowColor.x, ImGuiColorEditFlags_NoInputs);
            ch |= ImGui::SliderFloat2("Offset", &l.ShadowOffset.x, -0.05f, 0.05f, "%.3f");
            ch |= ImGui::SliderFloat("Blur", &l.ShadowBlur, 0.0f, 0.05f, "%.3f");
            ch |= ImGui::SliderFloat("Shadow opacity", &l.ShadowOpacity, 0.0f, 1.0f);
        }
        ch |= ImGui::SliderFloat("Chrome", &l.Metal, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Glow", &l.Glow, 0.0f, 1.0f);
        if (l.Type == Graffiti::LayerType::Stamp || l.Type == Graffiti::LayerType::Text) {
            ch |= ImGui::SliderFloat2("Position", &l.Position.x, 0.0f, 1.0f);
            ch |= ImGui::SliderFloat("Size", &l.Size, 0.02f, 1.2f);
            ch |= ImGui::SliderFloat("Rotation", &l.Rotation, -180.0f, 180.0f, "%.0f deg");
        }
        if (l.Type == Graffiti::LayerType::Stamp) {
            char tex[260];
            std::snprintf(tex, sizeof(tex), "%s", l.Texture.c_str());
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - 36.0f * m_UIScale);
            if (ImGui::InputText("##Image", tex, sizeof(tex))) { l.Texture = tex; ch = true; }
            if (ImGui::BeginDragDropTarget()) { // an image from the Asset Browser
                for (const char* type : {"ASSET_TEXTURE_PATH", "ASSET_FILE_PATH"})
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(type)) {
                        l.Texture = ProjectPaths::Relativize(std::string((const char*)pl->Data));
                        g.StampTexture = l.Texture;
                        ch = true;
                    }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (EditorUIPrimitives::SecondaryButton(ICON_FA_IMAGE "##pickStamp", ImVec2(30.0f * m_UIScale, 0))) {
                g.Picker.WantOpen = true;
                g.Picker.Target = g.Selected;
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Choose from the graffiti image library");
            ImGui::SameLine();
            ImGui::TextUnformatted("Image");
            ch |= ImGui::Checkbox("Opacity from brightness", &l.Luma);
            ch |= ImGui::Checkbox("Image's own colours", &l.TextureColor);
            ch |= ImGui::SliderFloat("Stretch", &l.Aspect, 0.25f, 4.0f);
        }
        if (l.Type == Graffiti::LayerType::Text) {
            char text[128];
            std::snprintf(text, sizeof(text), "%s", l.Text.c_str());
            if (ImGui::InputText("Text", text, sizeof(text))) {
                const std::string next = text;
                size_t common = 0;
                while (common < next.size() && common < l.Text.size() && next[common] == l.Text[common]) ++common;
                if (next.size() > l.Text.size()) { // letters typed: they cascade in
                    g.CascadeLayer = g.Selected;
                    g.CascadeFrom = (int)common;
                    g.CascadeStart = Now();
                }
                l.Text = next;
                ch = true;
            }
            ImGui::TextDisabled("Letters: %s", Graffiti::AlphabetCharacters(l.Alphabet));
            int alpha = 0;
            for (int a = 0; a < 4; ++a)
                if (l.Alphabet == Graffiti::kAlphabets[a]) alpha = a;
            if (ImGui::Combo("Alphabet", &alpha, Graffiti::kAlphabets, 4)) { l.Alphabet = Graffiti::kAlphabets[alpha]; ch = true; }
            ch |= ImGui::SliderFloat("Kerning", &l.Kerning, -0.3f, 0.5f);
            ch |= ImGui::SliderFloat("Jitter", &l.Jitter, 0.0f, 1.0f);
            static const char* warps[] = {"None", "Arc", "Wave"};
            int warp = (int)l.WarpMode;
            if (ImGui::Combo("Warp", &warp, warps, 3)) { l.WarpMode = (Graffiti::Warp)warp; ch = true; }
            if (l.WarpMode != Graffiti::Warp::None) ch |= ImGui::SliderFloat("Warp amount", &l.WarpAmount, 0.0f, 1.0f);
        }
        if (l.Type == Graffiti::LayerType::Spray) {
            ImGui::TextDisabled("%d strokes", (int)l.Strokes.size());
            if (!l.Strokes.empty() && EditorUIPrimitives::SecondaryButton("Clear strokes")) {
                l.Strokes.clear();
                ch = true;
            }
        }
        if (ch) {
            l.Color = Graffiti::Quantise(l.Color);
            l.GradientTo = Graffiti::Quantise(l.GradientTo);
            l.OutlineColor = Graffiti::Quantise(l.OutlineColor);
            l.ShadowColor = Graffiti::Quantise(l.ShadowColor);
            Edited(g);
        }
    }

    if (ImGui::CollapsingHeader("Piece")) {
        Graffiti::Piece& p = *g.Piece;
        bool ch = false;
        static const int sides[] = {512, 1024, 2048};
        static const char* sideNames[] = {"512", "1024", "2048"};
        int wi = p.Width == 512 ? 0 : p.Width == 1024 ? 1 : 2, hi = p.Height == 512 ? 0 : p.Height == 1024 ? 1 : 2;
        ImGui::SetNextItemWidth(90.0f * m_UIScale);
        if (ImGui::Combo("##cw", &wi, sideNames, 3)) { p.Width = sides[wi]; ch = true; }
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f * m_UIScale);
        if (ImGui::Combo("Canvas##ch", &hi, sideNames, 3)) { p.Height = sides[hi]; ch = true; }
        ch |= ImGui::DragFloat2("Size (m)", &p.Metres.x, 0.01f, 0.1f, 10.0f, "%.2f");
        int seed = (int)p.Seed;
        if (ImGui::InputInt("Seed", &seed)) { p.Seed = (uint32_t)std::max(seed, 0); ch = true; }
        ch |= ImGui::SliderFloat("Roughness", &p.Roughness, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Relief", &p.Relief, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Wall show-through", &p.ShowThrough, 0.0f, 1.0f);
        ch |= ImGui::Checkbox("Drips", &p.Drips);
        ch |= ImGui::ColorEdit3("Glow colour", &p.GlowColor.x, ImGuiColorEditFlags_NoInputs);
        ch |= ImGui::SliderFloat("Glow strength", &p.GlowStrength, 0.0f, 8.0f);
        ch |= ImGui::Checkbox("UV paint (glows under UV light only)", &p.UV);
        ImGui::TextDisabled("Weathering the decal starts with");
        ch |= ImGui::SliderFloat("Fade", &p.Fade, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Bleach", &p.Bleach, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Peel", &p.Peel, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Buff", &p.Buff, 0.0f, 1.0f);
        if (ch) {
            p.GlowColor = Graffiti::Quantise(p.GlowColor);
            Edited(g);
        }
    }
    ImGui::EndChild();

    // ---- The canvas ----
    ImGui::SameLine();
    ImGui::BeginChild("##gcanvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g.NeedsBuild && !g.Stroking && g.BakeStage != 1) Rebuild(g);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvasHit", ImVec2(std::max(avail.x, 8.0f), std::max(avail.y, 8.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiID panId = ImGui::GetID("##canvasPan");
    // Middle-drag pans (and flings), the wheel zooms about the cursor.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) M.InertiaStop(panId);
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        g.Pan = ImVec2(g.Pan.x + io.MouseDelta.x, g.Pan.y + io.MouseDelta.y);
        M.InertiaTrack(panId, io.MouseDelta);
        g.PanTracking = true;
    } else if (g.PanTracking) {
        g.PanTracking = false;
        M.InertiaRelease(panId);
    }
    const ImVec2 coast = M.InertiaDelta(panId);
    g.Pan = ImVec2(g.Pan.x + coast.x, g.Pan.y + coast.y);
    const float fit = std::min(avail.x / (float)g.Piece->Width, avail.y / (float)g.Piece->Height) * 0.95f;
    if (hovered && io.MouseWheel != 0.0f && !io.KeyCtrl) {
        const float before = g.Zoom;
        g.Zoom = std::clamp(g.Zoom * std::pow(1.15f, io.MouseWheel), 0.25f, 8.0f);
        const ImVec2 c(origin.x + avail.x * 0.5f + g.Pan.x, origin.y + avail.y * 0.5f + g.Pan.y);
        const float k = g.Zoom / before;
        g.Pan = ImVec2(g.Pan.x + (io.MousePos.x - c.x) * (1.0f - k), g.Pan.y + (io.MousePos.y - c.y) * (1.0f - k));
    } else if (hovered && io.MouseWheel != 0.0f && io.KeyCtrl) {
        g.DistanceCm = std::clamp(g.DistanceCm + io.MouseWheel * 2.0f, 2.0f, 60.0f);
    }
    const float scale = fit * g.Zoom;
    const ImVec2 size((float)g.Piece->Width * scale, (float)g.Piece->Height * scale);
    const ImVec2 c(origin.x + avail.x * 0.5f + g.Pan.x, origin.y + avail.y * 0.5f + g.Pan.y);
    g.CanvasMin = ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f);
    g.CanvasMax = ImVec2(c.x + size.x * 0.5f, c.y + size.y * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    if (g.Comp.PreviewTexture())
        dl->AddImage((ImTextureID)(intptr_t)g.Comp.PreviewTexture(), g.CanvasMin, g.CanvasMax, ImVec2(0, 1), ImVec2(1, 0));
    dl->AddRect(g.CanvasMin, g.CanvasMax, ImGui::GetColorU32(ImGuiCol_Border));
    // A sheen sweeps the canvas when a bake lands.
    const float sinceBake = (float)(Now() - g.BakeDoneAt);
    if (sinceBake < 0.9f) M.DrawSheen(dl, g.CanvasMin, g.CanvasMax, ImGui::GetID("##bakeSheen"), 1.0f - sinceBake / 0.9f);
    auto uvAt = [&](ImVec2 m) {
        return glm::vec2((m.x - g.CanvasMin.x) / std::max(size.x, 1.0f), (m.y - g.CanvasMin.y) / std::max(size.y, 1.0f));
    };
    const glm::vec2 uv = uvAt(io.MousePos);
    // The brush footprint under the cursor.
    if (hovered && (g.Active == GTool::Spray || g.Active == GTool::Erase)) {
        const Graffiti::CapModel& cm = Graffiti::CapFor(g.Cap);
        const float rMetres = std::max(cm.MinRadius, g.DistanceCm * 0.01f * cm.ConeTan);
        const float rPx = rMetres / std::max(g.Piece->Metres.x, 1e-3f) * size.x;
        dl->AddCircle(io.MousePos, rPx, IM_COL32(255, 255, 255, 140), 32, 1.0f);
        dl->AddCircle(io.MousePos, rPx * cm.Ring, IM_COL32(255, 255, 255, 50), 32, 1.0f);
    }

    // Hold Space over the canvas: a radial of caps and recent colours.
    g.RadialWasOpen = g.RadialOpen;
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Space, false) && !g.Stroking) {
        g.RadialOpen = true;
        g.RadialCenter = io.MousePos;
    }
    if (g.RadialOpen) {
        const int caps = 3, cols = std::min((int)g.Recent.size(), 6), n = caps + cols;
        const float bloom = M.AnimateToggle(ImGui::GetID("##radial"), true, g.RadialWasOpen, Glass::RevealOpen, Glass::RevealClose);
        const float R = 70.0f * m_UIScale * (0.6f + 0.4f * bloom);
        const float dx = io.MousePos.x - g.RadialCenter.x, dy = io.MousePos.y - g.RadialCenter.y;
        g.RadialHovered = -1;
        if (dx * dx + dy * dy > 18.0f * 18.0f) {
            float a = std::atan2(dy, dx) + 3.14159265f * 0.5f;
            if (a < 0) a += 6.2831853f;
            g.RadialHovered = (int)(a / (6.2831853f / (float)n) + 0.5f) % n;
        }
        dl->AddCircleFilled(g.RadialCenter, R + 26.0f * m_UIScale, IM_COL32(12, 12, 14, (int)(200 * bloom)), 48);
        static const char* capNames[] = {"Fat", "Skinny", "Calli"};
        for (int i = 0; i < n; ++i) {
            const float a = (float)i / (float)n * 6.2831853f - 3.14159265f * 0.5f;
            const ImVec2 p(g.RadialCenter.x + std::cos(a) * R, g.RadialCenter.y + std::sin(a) * R);
            const bool hot = g.RadialHovered == i;
            if (i < caps) {
                const bool cur = (int)g.Cap == i;
                dl->AddCircleFilled(p, 20.0f * m_UIScale, hot ? IM_COL32(255, 255, 255, 230) : IM_COL32(60, 60, 64, 230), 24);
                const ImVec2 ts = ImGui::CalcTextSize(capNames[i]);
                dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), hot ? IM_COL32(0, 0, 0, 255) : (cur ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 190, 190, 255)), capNames[i]);
            } else {
                const glm::vec3& rc = g.Recent[(size_t)(i - caps)];
                dl->AddCircleFilled(p, (hot ? 18.0f : 14.0f) * m_UIScale, ImGui::GetColorU32(ImVec4(rc.x, rc.y, rc.z, 1.0f)), 24);
                if (hot) dl->AddCircle(p, 20.0f * m_UIScale, IM_COL32(255, 255, 255, 255), 24, 2.0f);
            }
        }
        if (ImGui::IsKeyReleased(ImGuiKey_Space)) {
            if (g.RadialHovered >= 0 && g.RadialHovered < caps) g.Cap = (Graffiti::Cap)g.RadialHovered;
            else if (g.RadialHovered >= caps) g.Color = g.Recent[(size_t)(g.RadialHovered - caps)];
            if (g.RadialHovered >= 0) EditorSound::Play(EditorSound::Event::RadialPick);
            g.RadialOpen = false;
        }
    }

    // Tools.
    const bool lmbPressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g.RadialOpen;
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!g.StrokeInViewport || !g.Stroking) {
        switch (g.Active) {
            case GTool::Spray:
            case GTool::Erase:
                if (lmbPressed) BeginStroke(g, uv, g.Active == GTool::Erase, false);
                else if (g.Stroking && lmbDown) StrokeSample(g, uv);
                else if (g.Stroking && !lmbDown) EndStroke(g);
                break;
            case GTool::Move:
                if (lmbPressed && g.Selected >= 0) {
                    auto& l = L[(size_t)g.Selected];
                    if (l.Type == Graffiti::LayerType::Stamp || l.Type == Graffiti::LayerType::Text) {
                        g.Moving = true;
                        g.MoveOffset = l.Position - uv;
                    }
                } else if (g.Moving && lmbDown) {
                    L[(size_t)g.Selected].Position = uv + g.MoveOffset;
                    g.NeedsBuild = true;
                    g.Dirty = true;
                } else if (g.Moving) {
                    g.Moving = false;
                    SaveNow(g);
                }
                break;
            case GTool::Stamp:
            case GTool::Text:
                if (lmbPressed) {
                    Graffiti::Layer l;
                    const bool text = g.Active == GTool::Text;
                    l.Type = text ? Graffiti::LayerType::Text : Graffiti::LayerType::Stamp;
                    l.Name = text ? "Text " + std::to_string(L.size() + 1) : "Stamp " + std::to_string(L.size() + 1);
                    l.Color = Graffiti::Quantise(g.Color);
                    l.Position = uv;
                    if (text) {
                        l.Text = "TAG";
                        l.Outline = true;
                    } else {
                        l.Texture = g.StampTexture;
                        l.Luma = false; // the cooked graffiti packs carry their shape in alpha
                        l.Size = 0.35f;
                    }
                    const int at = g.Selected >= 0 ? g.Selected + 1 : (int)L.size();
                    L.insert(L.begin() + at, l);
                    g.LayerIds.insert(g.LayerIds.begin() + at, g.NextLayerId++);
                    g.Selected = at;
                    if (text) {
                        g.CascadeLayer = at;
                        g.CascadeFrom = 0;
                        g.CascadeStart = Now();
                    }
                    EditorSound::Play(EditorSound::Event::GraffitiLayerAdd);
                    Edited(g);
                    PushRecent(g, g.Color);
                }
                break;
            case GTool::Eyedropper:
                if (lmbPressed) {
                    g.Color = Graffiti::Quantise(g.Comp.SamplePreview(uv));
                    EditorSound::Play(EditorSound::Event::Click);
                }
                break;
            default: break;
        }
    }
    dl->PopClipRect();
    // Canvas footer.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + avail.y - ImGui::GetTextLineHeightWithSpacing()));
    ImGui::TextDisabled("%s  |  %dx%d  %.2fx%.2f m  |  %d layers  |  zoom %.0f%%  |  Space: caps & colours, Ctrl+wheel: distance",
                        ToolName(g.Active), g.Piece->Width, g.Piece->Height, g.Piece->Metres.x, g.Piece->Metres.y, (int)L.size(),
                        g.Zoom * 100.0f);
    ImGui::EndChild();

    {
        const std::string cur = g.Picker.Target >= 0 && g.Picker.Target < (int)L.size() ? L[(size_t)g.Picker.Target].Texture : g.StampTexture;
        const std::string pick = DrawStampPickerPopup(g, m_AssetThumbs, m_UIScale, cur);
        if (!pick.empty()) {
            g.StampTexture = pick;
            if (g.Picker.Target >= 0 && g.Picker.Target < (int)L.size() && L[(size_t)g.Picker.Target].Type == Graffiti::LayerType::Stamp) {
                L[(size_t)g.Picker.Target].Texture = pick;
                Edited(g);
            }
        }
    }
    ImGui::End();
}
