// The Animator window's shared pieces: its state, the per-frame context the three panels draw from,
// and the small widgets and helpers they share. Internal to EditorLayer_Animator*.cpp.
#pragma once

#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using AC = AnimatorController;

// --- graph nodes -------------------------------------------------------------------------------

// --- graph geometry --------------------------------------------------------------------------

constexpr float kStateW = 180.0f, kStateH = 42.0f;
constexpr float kSpecialW = 130.0f, kSpecialH = 34.0f;

enum class NodeKind { State, Entry, Any, Exit };
struct NodeRef {
    NodeKind Kind = NodeKind::State;
    int Index = -1;
    bool operator==(const NodeRef& o) const { return Kind == o.Kind && Index == o.Index; }
    bool operator<(const NodeRef& o) const { return Kind != o.Kind ? Kind < o.Kind : Index < o.Index; }
};

// --- window state ------------------------------------------------------------------------------


struct AnimatorWindowState {
    std::string Rel, Abs;
    fs::file_time_type Stamp{};
    double LastStat = 0.0;
    AC Doc;
    bool Loaded = false;
    std::string Error;
    std::string Saved; // the document as last saved: the undo baseline
    std::vector<std::string> Undo, Redo;

    entt::entity Entity = entt::null; // rig for clip pickers / live view
    int Layer = 0;
    int LeftTab = 0;

    // Selection. States can be multi-selected; everything else is single.
    NodeKind SelKind = NodeKind::State;
    std::vector<int> SelStates;
    bool SelSpecial = false;     // Entry / Any / Exit selected (SelKind says which)
    int SelTransition = -1;

    ImVec2 Pan{0.0f, 0.0f};
    float Zoom = 1.0f;
    bool FramePending = true;
    std::string CenterOn; // a state to pan to on the next draw (from the search box)

    bool Linking = false;
    NodeRef LinkFrom;
    bool BoxSelecting = false;
    ImVec2 BoxStart{};
    bool MovingNodes = false;
    NodeRef ContextNode;
    int ContextTransition = -1;
    glm::vec2 ContextWorld{0.0f};

    std::vector<std::pair<std::string, std::string>> Clips;
    const void* ClipsSource = nullptr;
    int ClipsModelCount = -1;

    AC::Layer& L() { return Doc.Layers[std::clamp(Layer, 0, (int)Doc.Layers.size() - 1)]; }
    void ClearSelection() { SelStates.clear(); SelSpecial = false; SelTransition = -1; }
    bool IsSelected(int s) const { return std::find(SelStates.begin(), SelStates.end(), s) != SelStates.end(); }

    bool Load(const std::string& rel) {
        Rel = rel;
        Abs = ProjectPaths::Resolve(rel);
        AC doc;
        std::string err;
        if (!AC::LoadFile(Abs, doc, &err)) { Loaded = false; Error = err; return false; }
        Doc = std::move(doc);
        Loaded = true;
        Error.clear();
        Saved = Doc.ToJsonString();
        std::error_code ec;
        Stamp = fs::last_write_time(fs::u8path(Abs), ec);
        Layer = std::clamp(Layer, 0, (int)Doc.Layers.size() - 1);
        ClearSelection();
        return true;
    }
    void Write() {
        if (!Doc.SaveFile(Abs)) { Log::Error("Animator: couldn't save " + Rel + "."); return; }
        std::error_code ec;
        Stamp = fs::last_write_time(fs::u8path(Abs), ec);
    }
    // Records the edit on the undo stack and saves. Call after any change is complete.
    void Commit() {
        std::string now = Doc.ToJsonString();
        if (now == Saved) return;
        Undo.push_back(std::move(Saved));
        if (Undo.size() > 200) Undo.erase(Undo.begin());
        Redo.clear();
        Saved = std::move(now);
        Write();
    }
    void Step(std::vector<std::string>& from, std::vector<std::string>& to) {
        if (from.empty()) return;
        to.push_back(Saved);
        Saved = std::move(from.back());
        from.pop_back();
        AC::FromJsonString(Saved, Doc);
        Layer = std::clamp(Layer, 0, (int)Doc.Layers.size() - 1);
        ClearSelection();
        Write();
    }
};

namespace AnimatorUI {

// The per-frame context the window's three panels draw from.
struct AnimCtx {
    World& world;
    AnimatorWindowState& W;
    AssetLibrary* assets = nullptr;
    AnimatorControllerComponent* live = nullptr; // the rig's runtime while playing
    Model* rig = nullptr;                        // the rig's model (clip pickers, previews)
    float S = 1.0f, leftW = 0.0f, rightW = 0.0f, bodyH = 0.0f;
    bool changed = false;                        // a discrete edit this frame: committed at the end
};

inline constexpr const char* kParamTypeLabels = "Float\0Int\0Bool\0Trigger\0";
inline constexpr const char* kOpLabels = "Greater\0Less\0Equals\0Not Equal\0Is True\0Is False\0";

extern char g_stateSearch[64]; // the top bar's find-state box

// std::string-backed InputText. True once the edit is committed.
bool InputName(const char* id, std::string& s, float width, bool allowEmpty = false);
bool RemoveButton(const char* tooltip);
std::string UniqueName(const std::string& base, const std::vector<std::string>& taken);
// Every clip this model can play, as (reference, label); AllClipChoices: every clip the project has loaded.
std::vector<std::pair<std::string, std::string>> ClipChoices(Model& model, AssetLibrary& assets);
std::vector<std::pair<std::string, std::string>> AllClipChoices(AssetLibrary& assets);
std::string ClipLabel(const std::string& ref);
// The pose of `st` on `track` at `phase` (0..1), blend trees at `params` (defaults for any missing).
bool SampleStatePose(Model& model, AssetLibrary& assets, const AnimatorController& D, const AnimatorController::State& st, int track,
                     float phase, const std::map<std::string, float>& params, int rootNode, const RootMotionSettings& rms,
                     std::vector<LocalTRS>& pose);
// Live parameter widget (Inspector section and the Animator window's Parameters tab).
void LiveParamWidget(AnimatorParam& p, float width);
// Removes states (and every transition touching them) from a layer.
void DeleteStates(AnimCtx& c, AC::Layer& Ly, std::vector<int> which);

void DrawLayersAndParameters(AnimCtx& cx); // left
void DrawGraphCanvas(AnimCtx& cx);         // middle
void DrawSelectionPanel(AnimCtx& cx);      // right

} // namespace AnimatorUI
