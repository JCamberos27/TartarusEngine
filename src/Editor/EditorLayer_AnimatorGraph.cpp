// The Animator window's middle panel: the state graph - nodes, edges, linking, selection, the context menu.
#include "EditorLayer.h"
#include "EditorTheme.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AnimatorLint.h"
#include "ClipAnalysis.h"
#include "FirstPersonBodyContract.h"
#include "FirstPersonAnimation.h"
#include "Components.h"
#include "CurveEditor.h"
#include "AssetLibrary.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <unordered_map>
#include "EditorLayer_AnimatorInternal.h"
#include "Glass/GlassMotion.h" // wire flow, node pops, the live-state glide, coasting pan
#include "Glass/GlassTokens.h"
#include <imgui_internal.h>    // ImHashStr

using namespace EditorInternal;
using namespace AnimatorUI;

namespace {

float Len(ImVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab = b - a;
    const float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return Len(p - (a + ab * t));
}

// Where the segment from the rect's centre toward `toward` leaves the rect.
ImVec2 RectExit(ImVec2 center, ImVec2 half, ImVec2 toward) {
    const ImVec2 d = toward - center;
    if (std::abs(d.x) < 1e-3f && std::abs(d.y) < 1e-3f) return center;
    const float tx = std::abs(d.x) > 1e-3f ? half.x / std::abs(d.x) : 1e9f;
    const float ty = std::abs(d.y) > 1e-3f ? half.y / std::abs(d.y) : 1e9f;
    const float t = std::min(1.0f, std::min(tx, ty));
    return center + d * t;
}

void Arrowhead(ImDrawList* dl, ImVec2 at, ImVec2 dir, float size, ImU32 col) {
    const float l = Len(dir);
    if (l < 1e-3f) return;
    dir = dir * (1.0f / l);
    const ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(at + dir * size, at - dir * size * 0.6f + n * size * 0.7f,
                          at - dir * size * 0.6f - n * size * 0.7f, col);
}

ImVec2 CubicAt(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float u) {
    const float v = 1.0f - u;
    return p0 * (v * v * v) + p1 * (3.0f * v * v * u) + p2 * (3.0f * v * u * u) + p3 * (u * u * u);
}

// A transition's conditions as one short line for its wire ("Speed > 0.50 & Grounded"); the first transition of
// a bundle, with "+N" for the rest.
std::string ConditionLabel(const AC::Layer& Ly, const std::vector<int>& transitions) {
    if (transitions.empty() || transitions.front() < 0 || transitions.front() >= (int)Ly.Transitions.size()) return {};
    const AC::Transition& t = Ly.Transitions[transitions.front()];
    std::string out;
    char buf[32];
    for (const auto& c : t.Conditions) {
        if (!out.empty()) out += " & ";
        switch (c.Mode) {
            case AC::Op::Greater:  std::snprintf(buf, sizeof buf, " > %.2f", c.Threshold); out += c.Param + buf; break;
            case AC::Op::Less:     std::snprintf(buf, sizeof buf, " < %.2f", c.Threshold); out += c.Param + buf; break;
            case AC::Op::Equals:   std::snprintf(buf, sizeof buf, " = %g", c.Threshold); out += c.Param + buf; break;
            case AC::Op::NotEqual: std::snprintf(buf, sizeof buf, " != %g", c.Threshold); out += c.Param + buf; break;
            case AC::Op::If:       out += c.Param; break;
            case AC::Op::IfNot:    out += "!" + c.Param; break;
        }
    }
    if (out.empty() && t.HasExitTime) { std::snprintf(buf, sizeof buf, "exit %.2f", t.ExitTime); out = buf; }
    if (transitions.size() > 1) { std::snprintf(buf, sizeof buf, "  +%d", (int)transitions.size() - 1); out += buf; }
    return out;
}

} // namespace

namespace AnimatorUI {

// Middle: the state graph - nodes, edges, linking, selection, the context menu.
void DrawGraphCanvas(AnimCtx& cx) {
    World& world = cx.world;
    AnimatorWindowState& W = cx.W;
    AC& D = W.Doc;
    AssetLibrary* assets = cx.assets;
    AnimatorControllerComponent* live = cx.live;
    Model* rig = cx.rig;
    const float S = cx.S, leftW = cx.leftW, rightW = cx.rightW, bodyH = cx.bodyH;
    bool& changed = cx.changed;
    (void)world; (void)assets; (void)live; (void)rig; (void)S; (void)leftW; (void)rightW; (void)bodyH; (void)D;
    // ================================ middle: the graph =======================================
    AC::Layer& Ly = W.L();
    const float canvasW = std::max(100.0f, ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##animcanvas", ImVec2(canvasW, bodyH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 csz = ImGui::GetContentRegionAvail();
    const ImVec2 c1 = c0 + csz;
    ImGui::InvisibleButton("##canvasbtn", csz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                   ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImGuiIO& io = ImGui::GetIO();

    auto nodeWorldPos = [&](NodeRef n) -> glm::vec2& {
        switch (n.Kind) {
            case NodeKind::Entry: return Ly.EntryPosition;
            case NodeKind::Any:   return Ly.AnyPosition;
            case NodeKind::Exit:  return Ly.ExitPosition;
            default:              return Ly.States[n.Index].Position;
        }
    };
    auto nodeSize = [&](NodeRef n) { return n.Kind == NodeKind::State ? ImVec2(kStateW, kStateH) : ImVec2(kSpecialW, kSpecialH); };
    auto toScreen = [&](glm::vec2 p) { return c0 + W.Pan + ImVec2(p.x, p.y) * W.Zoom; };
    auto toWorld = [&](ImVec2 s) { const ImVec2 v = (s - c0 - W.Pan) * (1.0f / W.Zoom); return glm::vec2(v.x, v.y); };
    auto nodeRect = [&](NodeRef n, ImVec2& a, ImVec2& b) {
        a = toScreen(nodeWorldPos(n));
        b = a + nodeSize(n) * W.Zoom;
    };
    auto nodeCenter = [&](NodeRef n) { ImVec2 a, b; nodeRect(n, a, b); return (a + b) * 0.5f; };

    std::vector<NodeRef> nodes;
    for (int i = 0; i < (int)Ly.States.size(); ++i) nodes.push_back({NodeKind::State, i});
    nodes.push_back({NodeKind::Entry, -1});
    nodes.push_back({NodeKind::Any, -1});
    nodes.push_back({NodeKind::Exit, -1});

    if (W.FramePending && csz.x > 10.0f) {
        W.FramePending = false;
        Glass::Motion().InertiaStop(GraphMotionSeed(W) ^ 0x9A11F11Bu); // a coasting pan stops for the framing
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (NodeRef n : nodes) {
            const glm::vec2 p = nodeWorldPos(n);
            const ImVec2 sz = nodeSize(n);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p + glm::vec2(sz.x, sz.y));
        }
        const glm::vec2 ext = hi - lo + glm::vec2(80.0f);
        // Down to the wheel's own minimum: a big graph in a small window still fits whole.
        W.Zoom = std::clamp(std::min(csz.x / ext.x, csz.y / ext.y), 0.1f, 1.5f);
        const glm::vec2 mid = (lo + hi) * 0.5f;
        W.Pan = csz * 0.5f - ImVec2(mid.x, mid.y) * W.Zoom;
    }

    if (!W.CenterOn.empty()) {
        const int ci = Ly.FindState(W.CenterOn);
        W.CenterOn.clear();
        Glass::Motion().InertiaStop(GraphMotionSeed(W) ^ 0x9A11F11Bu);
        if (ci >= 0 && csz.x > 10.0f) {
            const NodeRef cn{NodeKind::State, ci};
            const glm::vec2 p = nodeWorldPos(cn);
            const ImVec2 sz = nodeSize(cn);
            W.Pan = csz * 0.5f - ImVec2(p.x + sz.x * 0.5f, p.y + sz.y * 0.5f) * W.Zoom;
        }
    }

    // Motion v2. Every effect below is either a stateless function of the motion clock or a Glass entry that
    // lives only while it moves; at rest the graph makes no entries.
    Glass::ImAnimContext& M = Glass::Motion();
    const double now = M.Now();
    const unsigned seed = GraphMotionSeed(W);
    auto nodeId = [&](NodeRef n) -> unsigned {
        return n.Kind == NodeKind::State ? NodeMotionId(W, Ly.States[n.Index].Name) : seed + 0x51u * (unsigned)n.Kind;
    };
    // A middle-drag pan that was let go coasts on (Inertia); framing or centring on a state stops it.
    const ImGuiID panId = seed ^ 0x9A11F11Bu;
    // (FramePending and CenterOn above stop it.)
    W.Pan = W.Pan + M.InertiaDelta(panId);

    // Grid
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, EditorTheme::U32(EditorTheme::Field));
    {
        const float step = 40.0f * W.Zoom;
        if (step > 6.0f) {
            for (float x = std::fmod(W.Pan.x, step); x < csz.x; x += step)
                dl->AddLine(ImVec2(c0.x + x, c0.y), ImVec2(c0.x + x, c1.y), EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Text, 0.05f)));
            for (float y = std::fmod(W.Pan.y, step); y < csz.y; y += step)
                dl->AddLine(ImVec2(c0.x, c0.y + y), ImVec2(c1.x, c0.y + y), EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Text, 0.05f)));
        }
    }

    // --- edges -------------------------------------------------------------------------------
    struct Edge {
        NodeRef From, To;
        std::vector<int> Transitions;
        ImVec2 A, B;
    };
    std::map<std::pair<NodeRef, NodeRef>, int> edgeIndex;
    std::vector<Edge> edges;
    for (int t = 0; t < (int)Ly.Transitions.size(); ++t) {
        const auto& tr = Ly.Transitions[t];
        NodeRef from, to;
        if (tr.FromKind == AC::Source::Any) from = {NodeKind::Any, -1};
        else if (tr.FromKind == AC::Source::Entry) from = {NodeKind::Entry, -1};
        else {
            const int s = Ly.FindState(tr.From);
            if (s < 0) continue;
            from = {NodeKind::State, s};
        }
        if (tr.To == AC::kExitState) to = {NodeKind::Exit, -1};
        else {
            const int s = Ly.FindState(tr.To);
            if (s < 0) continue;
            to = {NodeKind::State, s};
        }
        auto key = std::make_pair(from, to);
        auto it = edgeIndex.find(key);
        if (it == edgeIndex.end()) {
            edgeIndex[key] = (int)edges.size();
            edges.push_back({from, to, {t}, {}, {}});
        } else {
            edges[it->second].Transitions.push_back(t);
        }
    }
    // The default-state arrow from Entry is implicit (no transition), drawn in orange.
    const int defaultState = Ly.DefaultStateIndex();

    // Wire morph: a node that jumped (undo, a typed position, a paste) has its wires ease to the new shape
    // (WireMorph) instead of redrawing at once. Last frame's position is kept in the canvas window's storage
    // (ImGui's memory); a direct mouse drag stays 1:1. The offset is screen px from the node's real centre.
    std::map<NodeRef, ImVec2> wireOff;
    {
        ImGuiStorage* st = ImGui::GetStateStorage();
        for (NodeRef n : nodes) {
            const glm::vec2 p = nodeWorldPos(n);
            const ImGuiID key = nodeId(n) ^ 0x3E0F7A11u;
            const ImVec2 prev = W.MovingNodes ? ImVec2(p.x, p.y) : ImVec2(st->GetFloat(key, p.x), st->GetFloat(key ^ 1u, p.y));
            st->SetFloat(key, p.x);
            st->SetFloat(key ^ 1u, p.y);
            const ImVec2 shown = M.Follow2(key, ImVec2(p.x, p.y), prev, Glass::WireMorph);
            if (std::abs(shown.x - p.x) > 0.01f || std::abs(shown.y - p.y) > 0.01f) wireOff[n] = (shown - ImVec2(p.x, p.y)) * W.Zoom;
        }
    }
    auto offOf = [&](NodeRef n) { auto it = wireOff.find(n); return it == wireOff.end() ? ImVec2(0, 0) : it->second; };
    auto wireCenter = [&](NodeRef n) { return nodeCenter(n) + offOf(n); };

    int hoveredEdge = -1;
    float hoveredEdgeDist = 7.0f;
    for (int e = 0; e < (int)edges.size(); ++e) {
        Edge& E = edges[e];
        ImVec2 a = wireCenter(E.From), b = wireCenter(E.To);
        if (E.From == E.To) continue; // self-loop: drawn as an arc below
        // A two-way pair gets a small sideways offset so the arrows don't overlap.
        if (edgeIndex.count({E.To, E.From})) {
            ImVec2 d = b - a;
            const float l = std::max(Len(d), 1e-3f);
            const ImVec2 n(-d.y / l, d.x / l);
            a = a + n * (6.0f * W.Zoom);
            b = b + n * (6.0f * W.Zoom);
        }
        ImVec2 ra, rb;
        nodeRect(E.From, ra, rb);
        const ImVec2 hA = (rb - ra) * 0.5f;
        nodeRect(E.To, ra, rb);
        const ImVec2 hB = (rb - ra) * 0.5f;
        E.A = RectExit(a, hA, b);
        E.B = RectExit(b, hB, a);
        if (canvasHovered) {
            const float d = SegmentDistance(mouse, E.A, E.B);
            if (d < hoveredEdgeDist) { hoveredEdgeDist = d; hoveredEdge = e; }
        }
    }
    auto edgeIsSelected = [&](const Edge& E) {
        return W.SelTransition >= 0 && std::find(E.Transitions.begin(), E.Transitions.end(), W.SelTransition) != E.Transitions.end();
    };
    const int liveTransition = live && W.Layer < (int)live->Layers.size() ? live->Layers[W.Layer].Transition : -1;
    // Live wire flow: the transition crossfading now, or the one most recently taken (an instant transition is never
    // "running", so the History tab's newest entry for this layer counts too), carries white pulses for a moment.
    {
        const unsigned flowKey = seed ^ (unsigned)(uintptr_t)live ^ ((unsigned)entt::to_integral(W.Entity) * 0x85EBCA6Bu);
        if (!live) { W.FlowTransition = -1; W.FlowKey = 0; }
        else {
            const AnimatorTransitionLog* newest = nullptr;
            for (auto it = live->History.rbegin(); it != live->History.rend(); ++it)
                if (it->Layer == W.Layer) { newest = &*it; break; }
            if (W.FlowKey != flowKey) { W.FlowKey = flowKey; W.FlowLogTime = newest ? newest->Time : -1.0f; W.FlowTransition = -1; }
            else if (newest && newest->Time != W.FlowLogTime) { W.FlowLogTime = newest->Time; W.FlowTransition = newest->Transition; W.FlowAt = now; }
            if (liveTransition >= 0) { W.FlowTransition = liveTransition; W.FlowAt = now; }
        }
    }
    constexpr float kFlowLingerSeconds = 1.2f; // a taken transition's pulses fade out over this
    const bool flowOn = live && M.CategoryOn(Glass::MotionCategory::Gradients);
    // Wire tug: the wire just linked sags and springs straight (NodePop's spring, riding on a pulse).
    const ImGuiID tugId = seed ^ 0x7E6A11C3u;
    float tugSag = 0.0f;
    if (W.TugTransition >= 0 && M.PulseProgress(tugId) < 1.0f)
        tugSag = Glass::SpringImpulse(M.PulseElapsed(tugId, 1.0e3f), Glass::NodePop.p0, Glass::NodePop.p1, Glass::NodePop.p2, 14.0f * W.Zoom);
    else W.TugTransition = -1;
    for (int e = 0; e < (int)edges.size(); ++e) {
        const Edge& E = edges[e];
        auto has = [&](int t) { return t >= 0 && std::find(E.Transitions.begin(), E.Transitions.end(), t) != E.Transitions.end(); };
        const bool sel = edgeIsSelected(E);
        const bool active = has(liveTransition);
        const ImU32 col = active ? EditorTheme::U32(EditorTheme::Info)
                        : sel ? ImGui::GetColorU32(EditorUIPrimitives::ActiveAccentColor())
                        : e == hoveredEdge ? EditorTheme::U32(EditorTheme::Text) : EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Secondary, 0.8f));
        const float thick = (sel || active ? 3.0f : 2.0f) * std::max(W.Zoom, 0.6f);
        if (E.From == E.To) {
            ImVec2 a, b;
            nodeRect(E.From, a, b);
            const ImVec2 top = ImVec2((a.x + b.x) * 0.5f, a.y) + offOf(E.From);
            const float r = 16.0f * W.Zoom;
            dl->AddBezierCubic(top + ImVec2(-r, 0), top + ImVec2(-r * 1.5f, -r * 2.5f), top + ImVec2(r * 1.5f, -r * 2.5f),
                               top + ImVec2(r, 0), col, thick);
            Arrowhead(dl, top + ImVec2(r, -2.0f * W.Zoom), ImVec2(0.3f, 1.0f), 7.0f * W.Zoom, col);
            if (canvasHovered && Len(mouse - (top + ImVec2(0, -r * 1.8f))) < r) hoveredEdge = e;
            continue;
        }
        // The wire as a cubic (straight unless it's being tugged), so the pulses and the label can ride it.
        const ImVec2 dir = E.B - E.A;
        const float l = std::max(Len(dir), 1e-3f);
        const ImVec2 nrm(-dir.y / l, dir.x / l);
        const float sag = has(W.TugTransition) ? tugSag : 0.0f;
        const ImVec2 p1 = E.A + dir * (1.0f / 3.0f) + nrm * (sag / 0.75f), p2 = E.A + dir * (2.0f / 3.0f) + nrm * (sag / 0.75f);
        if (sag != 0.0f) dl->AddBezierCubic(E.A, p1, p2, E.B, col, thick);
        else dl->AddLine(E.A, E.B, col, thick);
        const ImVec2 mid = (E.A + E.B) * 0.5f + nrm * sag;
        const float as = 7.0f * std::max(W.Zoom, 0.6f);
        if (E.Transitions.size() > 1) {
            // Several transitions between the same pair: three chevrons, like Unity.
            for (int k = -1; k <= 1; ++k) Arrowhead(dl, mid + dir * (k * 2.2f * as / l), dir, as, col);
        } else {
            Arrowhead(dl, mid, dir, as, col);
        }
        // The condition label, along the wire (always on: it's information). Two-way pairs put theirs on the outside;
        // TextAlongCubic skips a label that doesn't fit.
        if (W.Zoom >= 0.55f) {
            const std::string label = ConditionLabel(Ly, E.Transitions);
            if (!label.empty()) {
                EditorTheme::PushSmall();
                const float lift = ImGui::GetTextLineHeight() * 0.5f + 3.0f * std::max(W.Zoom, 0.6f);
                const bool pair = edgeIndex.count({E.To, E.From}) != 0;
                const float side = pair ? (E.A.x <= E.B.x ? -1.0f : 1.0f) : 1.0f;
                const ImU32 lc = sel || active || e == hoveredEdge ? col : EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Secondary, 0.75f));
                Glass::ImAnimContext::TextAlongCubic(dl, E.A, p1, p2, E.B, label.c_str(), lc, 0.5f, lift * side);
                EditorTheme::PopFont();
            }
        }
        // Flow pulses: emitted WireFlowSpeed times a second, each crossing the wire at a steady pace; a pure function
        // of the motion clock, so nothing is stored. Gradients off: none.
        float flow = active ? 1.0f : 0.0f;
        if (!active && has(W.FlowTransition)) flow = std::max(0.0f, 1.0f - (float)(now - W.FlowAt) / kFlowLingerSeconds);
        if (flowOn && flow > 0.0f) {
            const float travel = std::clamp(l / (240.0f * std::max(W.Zoom, 0.3f)), 0.45f, 1.6f); // seconds per crossing
            const float spacing = 1.0f / (Glass::WireFlowSpeed * travel);                      // in arc fraction
            const float head = (float)std::fmod(now / travel + (double)(e * 0.37f), 1.0);
            for (float u = std::fmod(head, spacing); u < 1.0f; u += spacing) {
                const ImVec2 p = CubicAt(E.A, p1, p2, E.B, u);
                const float a = flow * std::sin(u * 3.14159265f); // fade in and out at the ends
                dl->AddCircleFilled(p, 5.0f * std::max(W.Zoom, 0.6f), IM_COL32(255, 255, 255, (int)(40.0f * a)), 12);
                dl->AddCircleFilled(p, 2.4f * std::max(W.Zoom, 0.6f), IM_COL32(255, 255, 255, (int)(235.0f * a)), 10);
            }
        }
    }
    if (defaultState >= 0) {
        const NodeRef entry{NodeKind::Entry, -1}, def{NodeKind::State, defaultState};
        if (!edgeIndex.count({entry, def})) {
            ImVec2 ra, rb;
            nodeRect(entry, ra, rb);
            const ImVec2 a0 = (ra + rb) * 0.5f + offOf(entry), hA = (rb - ra) * 0.5f;
            nodeRect(def, ra, rb);
            const ImVec2 b0 = (ra + rb) * 0.5f + offOf(def), hB = (rb - ra) * 0.5f;
            const ImVec2 a = RectExit(a0, hA, b0), b = RectExit(b0, hB, a0);
            const ImU32 col = EditorTheme::U32(EditorTheme::Accent);
            dl->AddLine(a, b, col, 2.0f * std::max(W.Zoom, 0.6f));
            Arrowhead(dl, (a + b) * 0.5f, b - a, 7.0f * std::max(W.Zoom, 0.6f), col);
        }
    }

    // --- nodes -------------------------------------------------------------------------------
    NodeRef hoveredNode{NodeKind::State, -2};
    for (NodeRef n : nodes) {
        ImVec2 a, b;
        nodeRect(n, a, b);
        if (canvasHovered && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) hoveredNode = n;
    }
    if (hoveredNode.Index != -2) hoveredEdge = -1;

    // Live: which states are contributing, and how far through the current one we are.
    std::map<int, float> liveWeight;
    int liveCurrent = -1;
    float liveProgress = 0.0f;
    if (live && W.Layer < (int)live->Layers.size()) {
        const auto& rt = live->Layers[W.Layer];
        for (const auto& it : rt.Stack) liveWeight[it.State] = std::max(liveWeight[it.State], it.Fade);
        if (!rt.Stack.empty()) {
            liveCurrent = rt.Stack.back().State;
            const float ph = rt.Stack.back().Phase;
            const bool loop = liveCurrent >= 0 && liveCurrent < (int)Ly.States.size() && Ly.States[liveCurrent].Loop;
            liveProgress = loop ? ph - std::floor(ph) : std::min(ph, 1.0f);
        }
    }
    // A soft cue when the watched machine moves to another state - only while the Animator is hovered or focused, and
    // never for the first look at a rig / layer / controller (the tracker resets then, and whenever Play isn't live).
    if (liveCurrent >= 0) {
        const bool same = W.LiveSeen >= 0 && W.LiveSeenEntity == W.Entity && W.LiveSeenLayer == W.Layer && W.LiveSeenRel == W.Rel;
        const bool watched = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ||
                             ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (same && liveCurrent != W.LiveSeen && watched) EditorSound::Play(EditorSound::Event::LiveState);
        W.LiveSeen = liveCurrent;
        W.LiveSeenEntity = W.Entity;
        W.LiveSeenLayer = W.Layer;
        if (!same) W.LiveSeenRel = W.Rel;
    } else {
        W.LiveSeen = -1;
    }

    const float fontScale = std::clamp(W.Zoom, 0.5f, 1.4f);
    for (NodeRef n : nodes) {
        ImVec2 a, b;
        nodeRect(n, a, b);
        const float round = 6.0f * W.Zoom;
        ImU32 fill = EditorTheme::U32(EditorTheme::Pressed);
        // A new state pops in on the NodePop spring (from 85%, fading up): draw-only, through a layer.
        const ImGuiID popId = n.Kind == NodeKind::State ? nodeId(n) ^ 0x90F0B0B1u : 0;
        const float pop = popId ? M.AnimateToggle(popId, true, true, Glass::NodePop, Glass::NodePop) : 1.0f;
        const bool popping = popId && M.Live(popId);
        const int popVtx = popping ? Glass::ImAnimContext::BeginLayer(dl) : 0;
        std::string label;
        bool selected = false;
        switch (n.Kind) {
            case NodeKind::Entry: fill = EditorTheme::U32(EditorTheme::Rgb(0x2E, 0x6E, 0x44)); label = "Entry"; selected = W.SelSpecial && W.SelKind == NodeKind::Entry; break;
            case NodeKind::Any:   fill = EditorTheme::U32(EditorTheme::Rgb(0x2C, 0x5E, 0x6C)); label = "Any State"; selected = W.SelSpecial && W.SelKind == NodeKind::Any; break;
            case NodeKind::Exit:  fill = EditorTheme::U32(EditorTheme::Rgb(0x7E, 0x34, 0x30)); label = "Exit"; selected = W.SelSpecial && W.SelKind == NodeKind::Exit; break;
            default: {
                const auto& s = Ly.States[n.Index];
                label = s.Name;
                if (n.Index == defaultState) fill = EditorTheme::U32(EditorTheme::AccentDeep);
                bool blend = false;
                for (const auto& m : s.Motions) blend |= m.IsBlendTree();
                if (blend) label += "  " ICON_FA_SLIDERS;
                selected = W.IsSelected(n.Index);
                break;
            }
        }
        if (n.Kind == NodeKind::State) {
            auto lw = liveWeight.find(n.Index);
            if (lw != liveWeight.end()) {
                const float w = n.Index == liveCurrent ? 1.0f : lw->second * 0.6f;
                const ImVec4 base = ImGui::ColorConvertU32ToFloat4(fill);
                const ImVec4 blue(0.20f, 0.45f, 0.85f, 1.0f);
                fill = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x + (blue.x - base.x) * w, base.y + (blue.y - base.y) * w,
                                                             base.z + (blue.z - base.z) * w, 1.0f));
            }
        }
        dl->AddRectFilled(a + ImVec2(3, 4) * W.Zoom, b + ImVec2(3, 4) * W.Zoom, IM_COL32(0, 0, 0, 90), round);
        dl->AddRectFilled(a, b, fill, round);
        const ImU32 border = selected ? ImGui::GetColorU32(EditorUIPrimitives::ActiveAccentColor())
                           : (n == hoveredNode ? EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Secondary, 0.9f)) : EditorTheme::U32(EditorTheme::Hairline));
        dl->AddRect(a, b, border, round, 0, selected ? 2.5f : 1.0f);
        if (n.Kind == NodeKind::State && g_stateSearch[0] && ClipFilterMatch(g_stateSearch, label))
            dl->AddRect(a - ImVec2(3, 3), b + ImVec2(3, 3), EditorTheme::U32(EditorTheme::AccentBright), round + 2.0f, 0, 2.0f);
        if (n.Kind == NodeKind::State && n.Index == liveCurrent) {
            const float h = 4.0f * W.Zoom;
            dl->AddRectFilled(ImVec2(a.x + round, b.y - h - 3.0f * W.Zoom),
                              ImVec2(a.x + round + (b.x - a.x - 2 * round) * liveProgress, b.y - 3.0f * W.Zoom),
                              EditorTheme::U32(EditorTheme::Info));
        }
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * fontScale;
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
        // A state's tags ride under its name, small.
        std::string tagLine;
        if (n.Kind == NodeKind::State)
            for (const auto& t : Ly.States[n.Index].Tags) tagLine += (tagLine.empty() ? "" : "  ") + t;
        const float tfs = fs * 0.72f;
        const float lift = tagLine.empty() ? 0.0f : tfs * 0.55f;
        const ImVec2 tp((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f - lift);
        dl->PushClipRect(a, b, true);
        dl->AddText(font, fs, tp, EditorTheme::U32(EditorTheme::Text), label.c_str());
        if (!tagLine.empty()) {
            const ImVec2 tts = font->CalcTextSizeA(tfs, FLT_MAX, 0.0f, tagLine.c_str());
            dl->AddText(font, tfs, ImVec2((a.x + b.x - tts.x) * 0.5f, tp.y + ts.y), EditorTheme::U32(EditorTheme::Secondary), tagLine.c_str());
        }
        dl->PopClipRect();
        if (popping) {
            Glass::LayerTransform t;
            t.pivot = (a + b) * 0.5f;
            const float s = 0.85f + 0.15f * pop;
            t.scale = ImVec2(s, s);
            t.alpha = std::clamp(pop, 0.0f, 1.0f);
            Glass::ImAnimContext::EndLayer(dl, popVtx, t);
        }
    }

    // Deleted states shrink away where they stood, then their ghosts are dropped.
    for (size_t g = 0; g < W.Ghosts.size();) {
        const auto& gh = W.Ghosts[g];
        const float p = M.PulseProgress(gh.Id);
        if (p >= 1.0f) { W.Ghosts.erase(W.Ghosts.begin() + (std::ptrdiff_t)g); continue; }
        if (gh.Layer == W.Layer) {
            const ImVec2 a = toScreen(gh.Position), b = a + ImVec2(kStateW, kStateH) * W.Zoom;
            const float k = EditorTheme::Ease::OutCubic(p), round = 6.0f * W.Zoom;
            const int v0 = Glass::ImAnimContext::BeginLayer(dl);
            dl->AddRectFilled(a, b, EditorTheme::U32(EditorTheme::Pressed), round);
            dl->AddRect(a, b, EditorTheme::U32(EditorTheme::Hairline), round);
            const ImVec2 ts = ImGui::CalcTextSize(gh.Name.c_str());
            dl->PushClipRect(a, b, true);
            dl->AddText((a + b - ts) * 0.5f, EditorTheme::U32(EditorTheme::Text), gh.Name.c_str());
            dl->PopClipRect();
            Glass::LayerTransform t;
            t.pivot = (a + b) * 0.5f;
            t.scale = ImVec2(1.0f - 0.35f * k, 1.0f - 0.35f * k);
            t.alpha = 1.0f - k;
            Glass::ImAnimContext::EndLayer(dl, v0, t);
        }
        ++g;
    }

    // The live state's highlight glides from node to node (SelectionGlide) when the machine moves on, so the eye
    // follows the change. Tracked in world space: panning and zooming don't make it glide.
    if (liveCurrent >= 0 && liveCurrent < (int)Ly.States.size()) {
        const glm::vec2 target = Ly.States[liveCurrent].Position;
        const unsigned glideKey = seed ^ ((unsigned)entt::to_integral(W.Entity) * 0x27D4EB2Fu);
        const ImGuiID glideId = seed ^ 0x611DE5E1u;
        const bool from = W.GlideValid && W.GlideKey == glideKey && !W.MovingNodes;
        const ImVec2 prev = from ? ImVec2(W.GlideTarget.x, W.GlideTarget.y) : ImVec2(target.x, target.y);
        const ImVec2 at = M.Follow2(glideId, ImVec2(target.x, target.y), prev, Glass::SelectionGlide);
        W.GlideTarget = target;
        W.GlideValid = true;
        W.GlideKey = glideKey;
        const ImVec2 a = toScreen(glm::vec2(at.x, at.y)), b = a + ImVec2(kStateW, kStateH) * W.Zoom;
        const float round = 6.0f * W.Zoom, grow = 3.0f * std::max(W.Zoom, 0.6f);
        dl->AddRect(a - ImVec2(grow, grow), b + ImVec2(grow, grow), IM_COL32(255, 255, 255, 40), round + grow, 0, 4.0f * std::max(W.Zoom, 0.6f));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 220), round, 0, 2.0f * std::max(W.Zoom, 0.6f));
    } else {
        W.GlideValid = false;
    }

    // Link preview
    if (W.Linking) {
        const ImVec2 a = nodeCenter(W.LinkFrom);
        dl->AddLine(a, mouse, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::AccentBright, 0.85f)), 2.0f);
        Arrowhead(dl, mouse, mouse - a, 8.0f, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::AccentBright, 0.85f)));
    }
    // Box select
    if (W.BoxSelecting) {
        dl->AddRectFilled(W.BoxStart, mouse, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.12f)));
        dl->AddRect(W.BoxStart, mouse, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.7f)));
    }
    // Hint
    if (Ly.States.empty())
        dl->AddText(c0 + ImVec2(12, 10), EditorTheme::U32(EditorTheme::Dim), "Right-click to create a state.");
    else if (W.Linking)
        dl->AddText(c0 + ImVec2(12, 10), EditorTheme::U32(EditorTheme::Secondary), "Click a state (or Exit) to finish the transition. Esc cancels.");
    dl->PopClipRect();

    // --- interaction -------------------------------------------------------------------------
    // New states pop in (NodePop): the spring starts here and the node loop draws it. One NodePop cue per batch,
    // panned to where the first one lands.
    auto popIn = [&](const std::vector<int>& fresh) {
        bool cue = false;
        for (int i : fresh) {
            if (i < 0 || i >= (int)Ly.States.size()) continue;
            M.AnimateToggle(NodeMotionId(W, Ly.States[i].Name) ^ 0x90F0B0B1u, true, false, Glass::NodePop, Glass::NodePop);
            if (cue) continue;
            cue = true;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImVec2 at = toScreen(Ly.States[i].Position + glm::vec2(kStateW, kStateH) * 0.5f);
            EditorSound::Play(EditorSound::Event::NodePop, std::clamp((at.x - vp->Pos.x) / std::max(1.0f, vp->Size.x), 0.0f, 1.0f));
        }
    };
    auto makeTransition = [&](NodeRef from, NodeRef to) {
        if (to.Kind == NodeKind::Entry || to.Kind == NodeKind::Any) return;
        if (from.Kind == NodeKind::Exit) return;
        if (from.Kind == NodeKind::Entry && to.Kind == NodeKind::Exit) return;
        AC::Transition t;
        if (from.Kind == NodeKind::Any) t.FromKind = AC::Source::Any;
        else if (from.Kind == NodeKind::Entry) t.FromKind = AC::Source::Entry;
        else t.From = Ly.States[from.Index].Name;
        t.To = to.Kind == NodeKind::Exit ? std::string(AC::kExitState) : Ly.States[to.Index].Name;
        // Leaving a one-shot is usually "when it ends": start with exit time on.
        if (from.Kind == NodeKind::State && !Ly.States[from.Index].Loop) { t.HasExitTime = true; t.ExitTime = 1.0f; }
        if (from.Kind == NodeKind::Entry) t.Duration = 0.0f;
        Ly.Transitions.push_back(std::move(t));
        EditorSound::Play(EditorSound::Event::TransitionLink);
        W.TugTransition = (int)Ly.Transitions.size() - 1; // the new wire sags and springs straight
        M.Pulse(tugId, 0.6f);
        W.ClearSelection();
        W.SelTransition = (int)Ly.Transitions.size() - 1;
        changed = true;
    };
    auto deleteStates = [&](std::vector<int> which) { DeleteStates(cx, Ly, std::move(which)); };
    // withTransitions: the copies also get the transitions of the originals - between two copied states
    // they connect the copies; to or from a state that isn't copied they connect the copy to that state.
    auto duplicateStates = [&](bool withTransitions) {
        std::vector<std::string> names;
        for (const auto& s : Ly.States) names.push_back(s.Name);
        std::vector<int> fresh;
        std::vector<std::pair<std::string, std::string>> renamed; // original name -> copy name
        for (int s : W.SelStates) {
            if (s < 0 || s >= (int)Ly.States.size()) continue;
            AC::State copy = Ly.States[s];
            const std::string original = copy.Name;
            copy.Name = UniqueName(copy.Name, names);
            names.push_back(copy.Name);
            renamed.push_back({original, copy.Name});
            copy.Position += glm::vec2(30.0f, 30.0f);
            Ly.States.push_back(std::move(copy));
            fresh.push_back((int)Ly.States.size() - 1);
        }
        if (withTransitions) {
            auto copyOf = [&](const std::string& n) -> const std::string* {
                for (const auto& r : renamed) if (r.first == n) return &r.second;
                return nullptr;
            };
            const size_t count = Ly.Transitions.size();
            for (size_t i = 0; i < count; ++i) {
                const AC::Transition t = Ly.Transitions[i];
                const std::string* from = t.FromKind == AC::Source::State ? copyOf(t.From) : nullptr;
                const std::string* to = t.To == AC::kExitState ? nullptr : copyOf(t.To);
                if (!from && !to) continue;
                AC::Transition n = t;
                if (from) n.From = *from;
                if (to) n.To = *to;
                Ly.Transitions.push_back(std::move(n));
            }
        }
        if (!fresh.empty()) EditorSound::Play(EditorSound::Event::StateAdd);
        popIn(fresh);
        W.ClearSelection();
        W.SelStates = fresh;
        changed = true;
    };
    // Copy / paste: the selected states with the transitions between them, kept across controllers (the
    // clipboard outlives the window's controller). Parameters they use are not copied - Lint names any
    // the target lacks.
    struct Clipboard { std::vector<AC::State> States; std::vector<AC::Transition> Transitions; };
    static Clipboard s_clip;
    auto copyStates = [&]() {
        s_clip = {};
        for (int i : W.SelStates)
            if (i >= 0 && i < (int)Ly.States.size()) s_clip.States.push_back(Ly.States[i]);
        for (const auto& t : Ly.Transitions) {
            if (t.FromKind != AC::Source::State) continue;
            bool from = false, to = false;
            for (const auto& c : s_clip.States) { from |= c.Name == t.From; to |= c.Name == t.To; }
            if (from && to) s_clip.Transitions.push_back(t);
        }
    };
    auto pasteStates = [&]() {
        if (s_clip.States.empty()) return;
        std::vector<std::string> names;
        for (const auto& s : Ly.States) names.push_back(s.Name);
        std::vector<std::pair<std::string, std::string>> renamed;
        std::vector<int> fresh;
        for (AC::State copy : s_clip.States) {
            const std::string original = copy.Name;
            copy.Name = UniqueName(copy.Name, names);
            names.push_back(copy.Name);
            renamed.push_back({original, copy.Name});
            copy.Position += glm::vec2(30.0f, 30.0f);
            copy.Motions.resize(D.Tracks.size()); // the target may have a different number of tracks
            Ly.States.push_back(std::move(copy));
            fresh.push_back((int)Ly.States.size() - 1);
        }
        auto renamedTo = [&](const std::string& n) {
            for (const auto& r : renamed) if (r.first == n) return r.second;
            return n;
        };
        for (AC::Transition t : s_clip.Transitions) {
            t.From = renamedTo(t.From);
            t.To = renamedTo(t.To);
            Ly.Transitions.push_back(std::move(t));
        }
        EditorSound::Play(EditorSound::Event::StateAdd);
        popIn(fresh);
        W.ClearSelection();
        W.SelStates = fresh;
        changed = true;
    };
    auto createState = [&](glm::vec2 at, bool blendTree) {
        std::vector<std::string> names;
        for (const auto& s : Ly.States) names.push_back(s.Name);
        AC::State s;
        s.Name = UniqueName(blendTree ? "Blend Tree" : "New State", names);
        s.Motions.resize(D.Tracks.size());
        if (blendTree) {
            std::string param;
            for (const auto& p : D.Parameters) if (p.Type == AC::ParamType::Float) { param = p.Name; break; }
            s.Motions[0].BlendParam = param;
            s.Motions[0].Children = {{"", 0.0f, 1.0f}, {"", 1.0f, 1.0f}};
        }
        s.Position = at - glm::vec2(kStateW, kStateH) * 0.5f;
        Ly.States.push_back(std::move(s));
        EditorSound::Play(EditorSound::Event::StateAdd);
        popIn({(int)Ly.States.size() - 1});
        if (Ly.States.size() == 1) Ly.DefaultState = Ly.States[0].Name; // implicit: no DefaultState sound
        W.ClearSelection();
        W.SelStates = {(int)Ly.States.size() - 1};
        changed = true;
    };

    if (canvasHovered) {
        // Zoom about the mouse.
        if (io.MouseWheel != 0.0f) {
            const glm::vec2 before = toWorld(mouse);
            W.Zoom = std::clamp(W.Zoom * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.1f, 2.5f);
            const ImVec2 after = toScreen(before);
            W.Pan = W.Pan + (mouse - after);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (W.Linking) {
                if (hoveredNode.Index != -2) makeTransition(W.LinkFrom, hoveredNode);
                W.Linking = false;
            } else if (hoveredNode.Index != -2) {
                if (hoveredNode.Kind == NodeKind::State) {
                    if (io.KeyCtrl || io.KeyShift) {
                        auto it = std::find(W.SelStates.begin(), W.SelStates.end(), hoveredNode.Index);
                        if (it != W.SelStates.end()) W.SelStates.erase(it);
                        else W.SelStates.push_back(hoveredNode.Index);
                        W.SelSpecial = false;
                        W.SelTransition = -1;
                    } else if (!W.IsSelected(hoveredNode.Index)) {
                        W.ClearSelection();
                        W.SelStates = {hoveredNode.Index};
                    }
                } else {
                    W.ClearSelection();
                    W.SelSpecial = true;
                    W.SelKind = hoveredNode.Kind;
                }
                W.MovingNodes = true;
            } else if (hoveredEdge >= 0) {
                const Edge& E = edges[hoveredEdge];
                // Clicking a multi-transition arrow again cycles through its transitions.
                int pick = E.Transitions.front();
                auto it = std::find(E.Transitions.begin(), E.Transitions.end(), W.SelTransition);
                if (it != E.Transitions.end() && ++it != E.Transitions.end()) pick = *it;
                W.ClearSelection();
                W.SelTransition = pick;
            } else {
                if (!io.KeyCtrl && !io.KeyShift) W.ClearSelection();
                W.BoxSelecting = true;
                W.BoxStart = mouse;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (W.Linking) {
                W.Linking = false;
            } else {
                W.ContextNode = hoveredNode;
                W.ContextTransition = hoveredEdge >= 0 ? edges[hoveredEdge].Transitions.front() : -1;
                W.ContextWorld = toWorld(mouse);
                if (hoveredNode.Index != -2 && hoveredNode.Kind == NodeKind::State && !W.IsSelected(hoveredNode.Index)) {
                    W.ClearSelection();
                    W.SelStates = {hoveredNode.Index};
                }
                ImGui::OpenPopup("##animctx");
            }
        }
    }
    // Middle-drag pans 1:1 while tracking its velocity; let go and it flings and coasts (Inertia). A new press
    // catches a coast.
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) M.InertiaStop(panId);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) && ImGui::IsItemActive()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) W.Pan = W.Pan + io.MouseDelta;
        M.InertiaTrack(panId, io.MouseDelta); // a still pointer still samples, so holding before letting go doesn't fling
        W.PanTracking = true;
    } else if (W.PanTracking) {
        W.PanTracking = false;
        M.InertiaRelease(panId);
    }
    if (W.MovingNodes) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const glm::vec2 d(io.MouseDelta.x / W.Zoom, io.MouseDelta.y / W.Zoom);
            if (W.SelSpecial) nodeWorldPos({W.SelKind, -1}) += d;
            for (int s : W.SelStates) if (s >= 0 && s < (int)Ly.States.size()) Ly.States[s].Position += d;
        } else {
            W.MovingNodes = false;
            changed = true; // positions are saved (and undoable) once the drag ends
        }
    }
    if (W.BoxSelecting) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            W.BoxSelecting = false;
            const ImVec2 lo(std::min(W.BoxStart.x, mouse.x), std::min(W.BoxStart.y, mouse.y));
            const ImVec2 hi(std::max(W.BoxStart.x, mouse.x), std::max(W.BoxStart.y, mouse.y));
            if (hi.x - lo.x > 3.0f || hi.y - lo.y > 3.0f)
                for (int i = 0; i < (int)Ly.States.size(); ++i) {
                    ImVec2 a, b;
                    nodeRect({NodeKind::State, i}, a, b);
                    if (b.x >= lo.x && a.x <= hi.x && b.y >= lo.y && a.y <= hi.y && !W.IsSelected(i)) W.SelStates.push_back(i);
                }
        }
    }

    if (ImGui::BeginPopup("##animctx")) {
        const NodeRef n = W.ContextNode;
        if (n.Index != -2) {
            if (n.Kind != NodeKind::Exit && ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Make Transition")) {
                W.Linking = true;
                W.LinkFrom = n;
            }
            if (n.Kind == NodeKind::State) {
                if (ImGui::MenuItem(ICON_FA_STAR "  Set as Layer Default State", nullptr, false, n.Index != defaultState)) {
                    Ly.DefaultState = Ly.States[n.Index].Name;
                    EditorSound::Play(EditorSound::Event::DefaultState);
                    changed = true;
                }
                if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate", "Ctrl+D")) duplicateStates(false);
                if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate with transitions", "Ctrl+Shift+D")) duplicateStates(true);
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C")) copyStates();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del")) deleteStates(W.SelStates);
            }
        } else if (W.ContextTransition >= 0) {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Delete Transition")) {
                Ly.Transitions.erase(Ly.Transitions.begin() + W.ContextTransition);
                EditorSound::Play(EditorSound::Event::TransitionBreak);
                W.ClearSelection();
                changed = true;
            }
        } else {
            if (ImGui::MenuItem(ICON_FA_SQUARE_PLUS "  Create State")) createState(W.ContextWorld, false);
            if (ImGui::MenuItem(ICON_FA_SLIDERS "  Create Blend Tree State")) createState(W.ContextWorld, true);
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, !s_clip.States.empty())) pasteStates();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_EXPAND "  Frame All", "F")) W.FramePending = true;
        }
        ImGui::EndPopup();
    }

    // Keyboard, while the window has focus and no text field does.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { W.Linking = false; W.BoxSelecting = false; }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (!W.SelStates.empty()) deleteStates(W.SelStates);
            else if (W.SelTransition >= 0 && W.SelTransition < (int)Ly.Transitions.size()) {
                Ly.Transitions.erase(Ly.Transitions.begin() + W.SelTransition);
                EditorSound::Play(EditorSound::Event::TransitionBreak);
                W.ClearSelection();
                changed = true;
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !W.SelStates.empty()) duplicateStates(io.KeyShift);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !W.SelStates.empty()) copyStates();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) pasteStates();
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) W.FramePending = true;
    }
    ImGui::EndChild();
    ImGui::SameLine();

}

} // namespace AnimatorUI
