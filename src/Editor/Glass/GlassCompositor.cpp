#include "GlassCompositor.h"

#include "EditorUIPrimitives.h" // kHudPlateColor: the viewport overlays' plate, which becomes glass
#include "GLStateCache.h"
#include "GlassMotion.h"
#include "GlassTheme.h"
#include "Profiler.h"
#include "gl.h"

#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace Glass {

static bool Overlaps(const ImRect& a, const ImRect& b) {
    return a.Min.x < b.Max.x && b.Min.x < a.Max.x && a.Min.y < b.Max.y && b.Min.y < a.Max.y;
}

// What the window at the root of a dock tree is, for glass: a popup, a tooltip, a floating window
// (a modal, an undocked window or a floating dock node), or nothing that floats.
static SurfaceKind ClassifyTree(const ImGuiWindow* tree, const ImGuiViewport* vp) {
    if (!tree || tree->Viewport != vp) return SurfaceKind::None;
    if (tree->Flags & ImGuiWindowFlags_Tooltip) return SurfaceKind::Tooltip;
    if (tree->Flags & ImGuiWindowFlags_Popup)
        return (tree->Flags & ImGuiWindowFlags_Modal) ? SurfaceKind::Window : SurfaceKind::Popup;
    if (tree->Flags & ImGuiWindowFlags_ChildWindow) return SurfaceKind::None;
    if (tree->DockNodeAsHost) return tree->DockNodeAsHost->IsFloatingNode() ? SurfaceKind::Window : SurfaceKind::None;
    if (tree->DockIsActive) return SurfaceKind::None;
    if (tree->Flags & ImGuiWindowFlags_NoBackground) return SurfaceKind::None;
    // The menu bar and the status bar span the window and sit on its edges: chrome, solid.
    if (tree->Size.x >= vp->Size.x - 1.0f) return SurfaceKind::None;
    return SurfaceKind::Window;
}

// --- The compositor ----------------------------------------------------------------------------------

namespace {
constexpr ImGuiID kArriveSalt = 0x41525256u; // "ARRV"
constexpr ImGuiID kRimSalt    = 0x52494D47u; // "RIMG"

float Secs(const MotionSpec& s) { return s.durationMs * 0.001f; }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Seconds since `key`'s arrival started (restarting it when `appeared`), or `total` once it has
// finished: the pulse is gone then and nothing is held.
float ArrivalSeconds(ImGuiID key, float total, bool appeared) {
    ImAnimContext& m = Motion();
    if (appeared) m.Pulse(key, total);
    return m.PulseProgress(key) * total;
}
} // namespace

GlassCompositor::GlassCompositor() = default;

GlassCompositor::~GlassCompositor() { Release(); }

void GlassCompositor::Release() {
    m_Blur.Release();
    if (m_CaptureFbo) { glDeleteFramebuffers(1, &m_CaptureFbo); m_CaptureFbo = 0; }
    if (m_CaptureTex) { glDeleteTextures(1, &m_CaptureTex); m_CaptureTex = 0; }
    m_CaptureW = m_CaptureH = 0;
    m_BlurTex = 0;
    m_Plates.clear();
}

SurfacePose GlassCompositor::PoseFor(ImGuiWindow* group, SurfaceKind kind, bool appeared, float* rimBoost) {
    SurfacePose pose;
    *rimBoost = 0.0f;
    const ImRect r = group->Rect();
    const ImGuiID key = group->ID ^ kArriveSalt;
    const float px = Scale();
    switch (kind) {
    case SurfaceKind::Popup: {
        const float t = ArrivalSeconds(key, Secs(FloatIn), appeared);
        const float e = EaseProgress(FloatIn, t / Secs(FloatIn));
        pose.pivot = ImVec2((r.Min.x + r.Max.x) * 0.5f, r.Min.y);
        pose.scale = Lerp(FloatInFromScale, 1.0f, e);
        pose.offset = ImVec2(0.0f, -FloatInDropPx * px * (1.0f - e));
        pose.alpha = e;
        break;
    }
    case SurfaceKind::Tooltip: {
        const float t = ArrivalSeconds(key, Secs(TooltipIn), appeared);
        pose.alpha = EaseProgress(TooltipIn, t / Secs(TooltipIn));
        break;
    }
    case SurfaceKind::Window: {
        const float total = ImMax(Secs(WindowPop), Secs(WindowFade));
        const float t = ArrivalSeconds(key, total, appeared);
        pose.pivot = r.GetCenter();
        pose.scale = Lerp(WindowPopFromScale, 1.0f, EaseProgress(WindowPop, t / Secs(WindowPop)));
        pose.alpha = EaseProgress(WindowFade, t / Secs(WindowFade));
        break;
    }
    case SurfaceKind::Panel: {
        const float t = ArrivalSeconds(key, Secs(PanelIn), appeared);
        const float e = EaseProgress(PanelIn, t / Secs(PanelIn));
        pose.offset = ImVec2(0.0f, PanelInRisePx * px * (1.0f - e));
        pose.alpha = e;
        pose.moveClip = false; // the content rises inside the panel; the panel itself stays put
        break;
    }
    default:
        break;
    }
    if (kind == SurfaceKind::Popup || kind == SurfaceKind::Window || kind == SurfaceKind::Tooltip) {
        const float t = ArrivalSeconds(group->ID ^ kRimSalt, Secs(RimGlint), appeared);
        *rimBoost = RimGlintBoost * (1.0f - EaseProgress(RimGlint, t / Secs(RimGlint)));
    }
    // Snap the last sliver so a settled surface draws exactly where ImGui put it.
    if (std::fabs(pose.scale - 1.0f) < 1e-3f) pose.scale = 1.0f;
    if (std::fabs(pose.offset.y) < 0.05f) pose.offset.y = 0.0f;
    if (pose.alpha > 0.995f) pose.alpha = 1.0f;
    return pose;
}

void GlassCompositor::Capture(int w, int h) {
    PROFILE_GPU_SCOPE("Glass Capture");
    if (!m_CaptureTex || w != m_CaptureW || h != m_CaptureH) {
        if (m_CaptureFbo) glDeleteFramebuffers(1, &m_CaptureFbo);
        if (m_CaptureTex) glDeleteTextures(1, &m_CaptureTex);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_CaptureTex);
        glTextureStorage2D(m_CaptureTex, 1, GL_RGBA8, w, h);
        glTextureParameteri(m_CaptureTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_CaptureTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_CaptureTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_CaptureTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &m_CaptureFbo);
        glNamedFramebufferTexture(m_CaptureFbo, GL_COLOR_ATTACHMENT0, m_CaptureTex, 0);
        m_CaptureW = w;
        m_CaptureH = h;
    }
    // A blit honours the scissor test, and the UI pass may have left it on.
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBlitNamedFramebuffer(0, m_CaptureFbo, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    m_BlurTex = m_Blur.Update(m_CaptureTex, w, h, GlassBlurLevels, GlassSaturation, GlassCeiling);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLStateCache::Invalidate();
    ++m_StatCaptures;
}

void GlassCompositor::Flush(ImDrawData* dd, std::vector<ImDrawList*>& pending, bool& texturesSent) {
    if (pending.empty()) return;
    ImDrawData sub;
    sub.DisplayPos = dd->DisplayPos;
    sub.DisplaySize = dd->DisplaySize;
    sub.FramebufferScale = dd->FramebufferScale;
    sub.OwnerViewport = dd->OwnerViewport;
    // Texture uploads (the font atlas) ride on the first batch only: they're done after it.
    sub.Textures = texturesSent ? nullptr : dd->Textures;
    for (ImDrawList* l : pending) sub.AddDrawList(l);
    pending.clear();
    // AddDrawList no longer counts into the obsolete CmdListsCount (ImGui 1.92.9): read CmdLists.
    if (sub.CmdLists.Size == 0) return;
    sub.CmdListsCount = sub.CmdLists.Size;
    sub.Valid = true;
    ImGui_ImplOpenGL3_RenderDrawData(&sub);
    texturesSent = true;
    GLStateCache::Invalidate();
}

void GlassCompositor::Render(ImDrawData* dd, bool blur) {
    m_StatSurfaces = 0;
    m_StatCaptures = 0;
    if (!dd || !dd->Valid || dd->CmdLists.Size == 0 || dd->DisplaySize.x < 1.0f || dd->DisplaySize.y < 1.0f) {
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        return;
    }
    ImGuiContext& g = *GImGui;
    ImGuiViewport* vp = dd->OwnerViewport ? dd->OwnerViewport : ImGui::GetMainViewport();
    const int fbW = (int)(dd->DisplaySize.x * dd->FramebufferScale.x);
    const int fbH = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
    const ImVec2 dispMin = dd->DisplayPos;
    const ImVec2 dispMax(dd->DisplayPos.x + dd->DisplaySize.x, dd->DisplayPos.y + dd->DisplaySize.y);

    // Which window drew each list.
    std::unordered_map<const ImDrawList*, ImGuiWindow*> owner;
    owner.reserve((size_t)g.Windows.Size);
    for (ImGuiWindow* w : g.Windows)
        if (w->Active && !w->Hidden) owner[w->DrawList] = w;

    const ImU32 titleCols[3] = {ImGui::GetColorU32(ImGuiCol_TitleBg), ImGui::GetColorU32(ImGuiCol_TitleBgActive),
                                ImGui::GetColorU32(ImGuiCol_TitleBgCollapsed)};
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);

    // --- 1. Classify, convert and pose every list (CPU only) ---------------------------------------
    struct Group { SurfacePose Pose; float Rim = 0.0f; };
    std::unordered_map<ImGuiWindow*, Group> groups;
    std::vector<ListInfo> info((size_t)dd->CmdLists.Size);
    m_SeenNext.clear();
    for (int i = 0; i < dd->CmdLists.Size; ++i) {
        ImDrawList* dl = dd->CmdLists[i];
        ListInfo& li = info[(size_t)i];
        auto it = owner.find(dl);
        if (it == owner.end()) continue;
        ImGuiWindow* w = it->second;
        li.Window = w;
        ImGuiWindow* tree = w->RootWindowDockTree ? w->RootWindowDockTree : w;
        const SurfaceKind treeKind = ClassifyTree(tree, vp);
        bool glassList = false;   // part of a glass surface: its border becomes the glass edge
        bool clearOwnBg = false;  // and its own solid background goes too
        if (treeKind != SurfaceKind::None) {
            li.Group = tree;
            li.Kind = treeKind;
            glassList = true;
            clearOwnBg = w == tree || w->DockIsActive; // the surface itself, or a tab docked into it
            if (w == tree) {
                li.Plate = true;
                li.PlateRect = w->Rect();
                li.PlateRounding = w->WindowRounding;
            }
        } else {
            ImGuiWindow* root = w->RootWindow ? w->RootWindow : w;
            if (root->DockIsActive && root->Viewport == vp) {
                li.Group = root;
                li.Kind = SurfaceKind::Panel;
            }
            // An overlay plate inside a panel (the Scene view's tool rail): glass too.
            if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !(w->Flags & ImGuiWindowFlags_Popup) &&
                FindWindowBackground(dl, w->Rect()) == EditorUIPrimitives::kHudPlateColor) {
                glassList = clearOwnBg = true;
                li.Plate = true;
                li.PlateRect = w->Rect();
                li.PlateRounding = w->WindowRounding;
            }
        }

        // The pose of the surface this list belongs to (computed once per surface per frame).
        float rim = 0.0f;
        SurfacePose pose;
        if (li.Group) {
            auto git = groups.find(li.Group);
            if (git == groups.end()) {
                const SurfaceKind gk = li.Kind;
                const bool appeared = m_Seen.find(li.Group->ID) == m_Seen.end();
                Group gr;
                gr.Pose = PoseFor(li.Group, gk, appeared, &gr.Rim);
                git = groups.emplace(li.Group, gr).first;
            }
            m_SeenNext.insert(li.Group->ID);
            pose = git->second.Pose;
            rim = git->second.Rim;
        }

        if (glassList) {
            ImU32 clear[4] = {0, 0, 0, 0};
            int n = 0;
            if (clearOwnBg) {
                const ImU32 bg = FindWindowBackground(dl, w->Rect());
                if (bg != 0) clear[n++] = bg;
                else if (li.Plate && !(w->DockNodeAsHost)) li.Plate = false; // drew no background: an overlay, keep it bare
                for (ImU32 c : titleCols) clear[n++] = c;
            }
            const ImVec4 edge = WithAlpha(GlassEdge, ImMin(1.0f, GlassEdge.w + rim));
            ConvertToGlass(dl, clear, n, border, U32(edge));
        }
        // Whatever solid fills are left (component headers, buttons, rows, hairlines in a panel,
        // the menu bar, a popup's items) become glass washes too. The Scene and Game views are
        // left exactly as drawn: the 3D image, the gizmos and the toolbar are content.
        {
            const ImGuiWindow* root = w->RootWindow ? w->RootWindow : w;
            if (!std::strstr(root->Name, "###Scene") && !std::strstr(root->Name, "###Game")) GlassifySolids(dl);
        }
        if (li.Plate) {
            li.PlateRect = pose.Apply(li.PlateRect);
            li.PlateRounding *= pose.scale;
            li.Alpha = pose.alpha;
        }
        // A docked panel keeps its own background in place while its content arrives.
        const ImU32 anchored = (li.Kind == SurfaceKind::Panel && li.Group == w) ? FindWindowBackground(dl, w->Rect()) : 0u;
        ApplyPose(dl, pose, anchored);
    }
    m_Seen.swap(m_SeenNext);

    // --- 2. Render back to front, laying glass under each surface ----------------------------------
    std::vector<ImDrawList*> pending;
    pending.reserve((size_t)dd->CmdLists.Size + 8);
    std::vector<ImRect> dirty; // what has been drawn since the last capture
    bool haveCapture = false, texturesSent = false;
    size_t plateCount = 0;
    const ImU32 fallback = U32(GlassFallback);
    for (int i = 0; i < dd->CmdLists.Size; ++i) {
        const ListInfo& li = info[(size_t)i];
        if (li.Plate && li.Alpha > 0.0f && li.PlateRect.GetWidth() >= 1.0f && li.PlateRect.GetHeight() >= 1.0f) {
            ++m_StatSurfaces;
            const ImRect r = li.PlateRect;
            const bool tooltip = li.Kind == SurfaceKind::Tooltip;
            const float spread = li.Kind == SurfaceKind::Window ? Px(Space3) : tooltip ? 0.0f : Px(Space1h);
            ImRect reach(r.Min.x - spread, r.Min.y - spread, r.Max.x + spread, r.Max.y + spread * 1.4f);
            reach.ClipWithFull(ImRect(dispMin, dispMax));

            bool useBlur = blur;
            if (useBlur) {
                bool stale = !haveCapture;
                for (const ImRect& d : dirty) stale = stale || Overlaps(d, r);
                if (stale && m_StatCaptures < kMaxCapturesPerFrame) {
                    Flush(dd, pending, texturesSent);
                    Capture(fbW, fbH);
                    haveCapture = m_BlurTex != 0;
                    dirty.clear();
                }
                useBlur = haveCapture;
            }

            if (plateCount >= m_Plates.size())
                m_Plates.push_back(std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData()));
            ImDrawList* plate = m_Plates[plateCount++].get();
            plate->_ResetForNewFrame();
            plate->Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex |
                           ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AllowVtxOffset;
            plate->PushTexture(ImGui::GetIO().Fonts->TexRef);
            plate->PushClipRect(reach.Min, reach.Max);
            const float a = li.Alpha;
            if (spread > 0.0f) DrawSoftShadow(plate, r.Min, r.Max, li.PlateRounding, 0.35f * a, spread);
            if (useBlur) {
                const ImVec2 uv0((r.Min.x - dispMin.x) / dd->DisplaySize.x, 1.0f - (r.Min.y - dispMin.y) / dd->DisplaySize.y);
                const ImVec2 uv1((r.Max.x - dispMin.x) / dd->DisplaySize.x, 1.0f - (r.Max.y - dispMin.y) / dd->DisplaySize.y);
                plate->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)m_BlurTex), r.Min, r.Max, uv0, uv1,
                                       IM_COL32(255, 255, 255, (int)(255.0f * a + 0.5f)), li.PlateRounding);
                plate->AddRectFilled(r.Min, r.Max, U32(WithAlpha(GlassTint, GlassTint.w * a)), li.PlateRounding);
            } else {
                plate->AddRectFilled(r.Min, r.Max, (fallback & ~IM_COL32_A_MASK) |
                                     ((ImU32)(255.0f * a + 0.5f) << IM_COL32_A_SHIFT), li.PlateRounding);
            }
            plate->PopClipRect();
            plate->PopTexture();
            pending.push_back(plate);
            dirty.push_back(reach);
        }
        ImDrawList* dl = dd->CmdLists[i];
        pending.push_back(dl);
        const ImRect b = DrawListBounds(dl);
        if (b.Min.x < b.Max.x) dirty.push_back(b);
    }
    Flush(dd, pending, texturesSent);
}

} // namespace Glass
