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
#include <cstdlib>

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

// --- The startup sequence ----------------------------------------------------------------------------

bool StartupPosing() {
    const ImAnimContext& m = Motion();
    return m.StartupRunning() && m.CategoryOn(MotionCategory::Startup);
}

ImGuiWindow* StartupGroup(ImGuiWindow* w, const ImGuiViewport* vp) {
    if (!w || w->Viewport != vp) return nullptr;
    ImGuiWindow* tree = w->RootWindowDockTree ? w->RootWindowDockTree : w;
    const SurfaceKind k = ClassifyTree(tree, vp);
    if (k == SurfaceKind::Window) return tree;
    if (k != SurfaceKind::None) return nullptr; // popups and tooltips arrive on their own
    ImGuiWindow* root = w->RootWindow ? w->RootWindow : w;
    return root->DockIsActive && root->Viewport == vp ? root : nullptr;
}

SurfacePose StartupPoseFor(const ImGuiWindow* group, const ImGuiViewport* vp) {
    const ImVec2 dMin = vp->Pos, dMax(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    const ImRect r = group->Rect();
    return StartupPose(r, dMin, dMax, StartupLocal(Motion().StartupElapsed(), StartupDelay(r, dMin, dMax)));
}

void PoseStartupTabBars(const ImGuiViewport* vp) {
    ImGuiContext& g = *GImGui;
    for (int n = 0; n < g.DockContext.Nodes.Data.Size; ++n) {
        ImGuiDockNode* node = (ImGuiDockNode*)g.DockContext.Nodes.Data[n].val_p;
        if (!node || !node->TabBar || !node->HostWindow || !node->IsVisible || !node->VisibleWindow) continue;
        if (node->IsHiddenTabBar() || node->IsNoTabBar() || node->HostWindow->Viewport != vp) continue;
        // A floating node's host is a floating window, posed whole with everything in it.
        if (StartupGroup(node->HostWindow, vp) != nullptr) continue;
        ImGuiWindow* panel = node->VisibleWindow->RootWindow ? node->VisibleWindow->RootWindow : node->VisibleWindow;
        const SurfacePose pose = StartupPoseFor(panel, vp);
        if (!pose.Identity()) ApplyPoseInRect(node->HostWindow->DrawList, pose, node->TabBar->BarRect);
    }
}

void ApplyStartupArrival() {
    if (!StartupPosing()) return;
    ImGuiContext& g = *GImGui;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    for (ImGuiWindow* w : g.Windows) {
        if (!w->Active || w->Hidden || !w->DrawList || w->DrawList->VtxBuffer.Size == 0) continue;
        if (ImGuiWindow* group = StartupGroup(w, vp)) ApplyPose(w->DrawList, StartupPoseFor(group, vp));
    }
    PoseStartupTabBars(vp);
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

void GlassCompositor::Capture(int w, int h, const CaptureRect& rect) {
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
    // A blit honours the scissor test, and the UI pass may have left it on. Only the rect: the copy
    // keeps the framebuffer's coordinates, and the blur reads nothing outside it.
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBlitNamedFramebuffer(0, m_CaptureFbo, rect.X0, rect.Y0, rect.X1, rect.Y1, rect.X0, rect.Y0, rect.X1, rect.Y1,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    m_BlurTex = m_Blur.UpdateRect(m_CaptureTex, w, h, rect.X0, rect.Y0, rect.X1 - rect.X0, rect.Y1 - rect.Y0, GlassBlurLevels,
                                  GlassSaturation, GlassCeiling);
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

bool FullFrameBlur() {
    static const bool on = [] {
        char* e = nullptr;
        size_t n = 0;
        const bool set = _dupenv_s(&e, &n, "TARTARUS_GLASS_FULL_CAPTURE") == 0 && e && *e && *e != '0';
        std::free(e);
        return set;
    }();
    return on;
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
    const int listCount = dd->CmdLists.Size;

    // Which window drew each list (sorted, looked up by binary search).
    m_Owner.clear();
    for (ImGuiWindow* w : g.Windows)
        if (w->Active && !w->Hidden) m_Owner.emplace_back(w->DrawList, w);
    std::sort(m_Owner.begin(), m_Owner.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto ownerOf = [&](const ImDrawList* dl) -> ImGuiWindow* {
        const auto it = std::lower_bound(m_Owner.begin(), m_Owner.end(), dl, [](const auto& e, const ImDrawList* k) { return e.first < k; });
        return it != m_Owner.end() && it->first == dl ? it->second : nullptr;
    };

    const ImU32 titleCols[3] = {ImGui::GetColorU32(ImGuiCol_TitleBg), ImGui::GetColorU32(ImGuiCol_TitleBgActive),
                                ImGui::GetColorU32(ImGuiCol_TitleBgCollapsed)};
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);

    // --- 1. Classify, convert and pose every list (CPU only) ---------------------------------------
    // The startup sequence: every docked panel and floating window assembles from its own pose (in
    // place of its ordinary arrival), and each dock node's tab bar, drawn in the dock host's list,
    // moves with its panel.
    const bool startup = StartupPosing();
    if (startup) PoseStartupTabBars(vp);
    m_Groups.clear();
    m_Info.assign((size_t)listCount, ListInfo{});
    m_Bounds.resize((size_t)listCount);
    m_SeenNext.clear();
    for (int i = 0; i < listCount; ++i) {
        ImDrawList* dl = dd->CmdLists[i];
        ListInfo& li = m_Info[(size_t)i];
        ImGuiWindow* w = ownerOf(dl);
        if (!w) {
            m_Bounds[(size_t)i] = DrawListBounds(dl);
            continue;
        }
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
            auto git = std::find_if(m_Groups.begin(), m_Groups.end(), [&](const Group& gr) { return gr.Window == li.Group; });
            if (git == m_Groups.end()) {
                const bool appeared = m_Seen.find(li.Group->ID) == m_Seen.end();
                Group gr{li.Group, SurfacePose{}, 0.0f, false};
                gr.Pose = PoseFor(li.Group, li.Kind, appeared, &gr.Rim);
                if (startup && (li.Kind == SurfaceKind::Panel || li.Kind == SurfaceKind::Window)) {
                    gr.Pose = StartupPoseFor(li.Group, vp);
                    gr.Startup = true;
                    // A floating window's glass edge catches the light as it lands, like the panels' rims.
                    const ImRect gr0 = li.Group->Rect();
                    const float since = Motion().StartupElapsed() - StartupDelay(gr0, dispMin, dispMax) -
                                        StartupPanelMs * 0.001f * StartupRimAtShare;
                    if (since >= 0.0f)
                        gr.Rim = ImMax(gr.Rim, StartupRimBoost * (1.0f - EaseProgress(RimGlint, since / Secs(RimGlint))));
                }
                m_Groups.push_back(gr);
                git = m_Groups.end() - 1;
            }
            m_SeenNext.insert(li.Group->ID);
            pose = git->Pose;
            rim = git->Rim;
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
        // left exactly as drawn: the 3D image, the gizmos and the overlays are content. The Game
        // view's own strip above its image (resolution, scale) is chrome, so it becomes glass too.
        {
            const ImGuiWindow* root = w->RootWindow ? w->RootWindow : w;
            const bool scene = std::strstr(root->Name, "###Scene") != nullptr;
            const bool gameImage = std::strstr(root->Name, "###Game") != nullptr && w == root;
            if (!scene && !gameImage) GlassifySolids(dl, m_MaterialBlend);
        }
        if (li.Plate) {
            li.PlateRect = pose.Apply(li.PlateRect);
            li.PlateRounding *= pose.scale;
            li.Alpha = pose.alpha;
        }
        // A docked panel keeps its own background in place while its content arrives.
        // (Not while it assembles at startup: then the whole panel arrives, background and all.)
        bool startupPosed = false;
        for (const Group& gr : m_Groups) if (gr.Window == li.Group) { startupPosed = gr.Startup; break; }
        const ImU32 anchored = (li.Kind == SurfaceKind::Panel && li.Group == w && !startupPosed) ? FindWindowBackground(dl, w->Rect()) : 0u;
        ApplyPose(dl, pose, anchored);
        m_Bounds[(size_t)i] = DrawListBounds(dl);
    }
    m_Seen.swap(m_SeenNext);

    // --- 2. Plan the captures (CPU only) -----------------------------------------------------------
    // The same walk as the render below: which capture each plate samples, and so the part of the
    // frame each capture has to blur - the union of its plates. A plate past kMaxCapturesPerFrame reuses
    // the last capture, so it widens that one.
    auto drawsPlate = [&](const ListInfo& li) {
        return li.Plate && li.Alpha > 0.0f && li.PlateRect.GetWidth() >= 1.0f && li.PlateRect.GetHeight() >= 1.0f;
    };
    m_PlateCapture.assign((size_t)listCount, -1);
    m_CaptureUse.clear();
    if (blur) {
        m_Dirty.clear();
        for (int i = 0; i < listCount; ++i) {
            const ListInfo& li = m_Info[(size_t)i];
            if (drawsPlate(li)) {
                const ImRect& r = li.PlateRect;
                bool stale = m_CaptureUse.empty();
                for (const ImRect& d : m_Dirty) stale = stale || Overlaps(d, r);
                if (stale && (int)m_CaptureUse.size() < kMaxCapturesPerFrame) {
                    m_CaptureUse.push_back(r);
                    m_Dirty.clear();
                } else {
                    m_CaptureUse.back().Add(r);
                }
                m_PlateCapture[(size_t)i] = (int)m_CaptureUse.size() - 1;
                ImRect reach = r;
                reach.ClipWithFull(ImRect(dispMin, dispMax));
                m_Dirty.push_back(reach);
            }
            const ImRect& b = m_Bounds[(size_t)i];
            if (b.Min.x < b.Max.x) m_Dirty.push_back(b);
        }
    }
    // Each capture's framebuffer rect (GL pixels, bottom-up): its plates, out by the blur's reach, on the
    // grid of the blur's coarsest level so the result doesn't shift as a surface moves.
    m_CaptureRects.clear();
    {
        const int reach = GlassBackdrop::BlurReach(GlassBlurLevels);
        const int grid = 1 << std::clamp(GlassBlurLevels, 1, 8);
        const float sx = dd->FramebufferScale.x, sy = dd->FramebufferScale.y;
        for (const ImRect& u : m_CaptureUse) {
            CaptureRect c{0, 0, fbW, fbH};
            if (!FullFrameBlur()) {
                const float x0 = (u.Min.x - dispMin.x) * sx, x1 = (u.Max.x - dispMin.x) * sx;
                const float y0 = (float)fbH - (u.Max.y - dispMin.y) * sy, y1 = (float)fbH - (u.Min.y - dispMin.y) * sy;
                auto down = [&](float v) { return (int)std::floor((v - (float)reach) / (float)grid) * grid; };
                auto up = [&](float v) { return (int)std::ceil((v + (float)reach) / (float)grid) * grid; };
                c = {std::max(down(x0), 0), std::max(down(y0), 0), std::min(up(x1), fbW), std::min(up(y1), fbH)};
                if (c.X1 - c.X0 < 8 || c.Y1 - c.Y0 < 8) c = {0, 0, fbW, fbH};
            }
            m_CaptureRects.push_back(c);
        }
    }

    // --- 3. Render back to front, laying glass under each surface ----------------------------------
    m_Pending.clear();
    int captured = -1;            // the capture the blur texture holds now
    bool texturesSent = false;
    size_t plateCount = 0;
    const ImU32 fallback = U32(GlassFallback);
    for (int i = 0; i < listCount; ++i) {
        const ListInfo& li = m_Info[(size_t)i];
        if (drawsPlate(li)) {
            ++m_StatSurfaces;
            const ImRect r = li.PlateRect;
            // Everything that floats (popups, menus, tooltips, floating windows, the Scene view's
            // overlays) is the same glass as the editor's chrome: the chrome's dark tint over the
            // blur and a faint edge, with no drop shadow.
            const ImVec4 tint = OverlayTint;
            ImRect reach = r;
            reach.ClipWithFull(ImRect(dispMin, dispMax));

            const int k = m_PlateCapture[(size_t)i];
            if (k >= 0 && k != captured) {
                Flush(dd, m_Pending, texturesSent);
                Capture(fbW, fbH, m_CaptureRects[(size_t)k]);
                captured = k;
            }
            const bool useBlur = k >= 0 && m_BlurTex != 0;

            if (plateCount >= m_Plates.size())
                m_Plates.push_back(std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData()));
            ImDrawList* plate = m_Plates[plateCount++].get();
            plate->_ResetForNewFrame();
            plate->Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex |
                           ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AllowVtxOffset;
            plate->PushTexture(ImGui::GetIO().Fonts->TexRef);
            plate->PushClipRect(reach.Min, reach.Max);
            const float a = li.Alpha;
            if (useBlur) {
                // The plate's corners in framebuffer pixels (bottom-up), then into the blurred rect.
                const float sx = dd->FramebufferScale.x, sy = dd->FramebufferScale.y;
                const ImVec2 uv0(m_Blur.OutU((r.Min.x - dispMin.x) * sx), m_Blur.OutV((float)fbH - (r.Min.y - dispMin.y) * sy));
                const ImVec2 uv1(m_Blur.OutU((r.Max.x - dispMin.x) * sx), m_Blur.OutV((float)fbH - (r.Max.y - dispMin.y) * sy));
                plate->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)m_BlurTex), r.Min, r.Max, uv0, uv1,
                                       IM_COL32(255, 255, 255, (int)(255.0f * a + 0.5f)), li.PlateRounding);
                plate->AddRectFilled(r.Min, r.Max, U32(WithAlpha(tint, tint.w * a)), li.PlateRounding);
            } else {
                plate->AddRectFilled(r.Min, r.Max, (fallback & ~IM_COL32_A_MASK) |
                                     ((ImU32)(255.0f * a + 0.5f) << IM_COL32_A_SHIFT), li.PlateRounding);
            }
            plate->PopClipRect();
            plate->PopTexture();
            m_Pending.push_back(plate);
        }
        m_Pending.push_back(dd->CmdLists[i]);
    }
    Flush(dd, m_Pending, texturesSent);
}

} // namespace Glass
