// The editor side of clustered decals (docs/DECALS.md, "Editor & assets"): the .decal asset
// editor, the Decal component's Inspector extras (a preview with the live atlas-frame picker, the
// channel matrix, the fade curves, Fit to Surface), Create Decal and the albedo an Asset Browser
// tile shows. Placing a dragged .decal and the projection-box wire live with the rest of the
// viewport code in EditorLayer_Gizmos.cpp; the component's fields and its Randomize Seed button
// are reflected (ComponentRegistry.cpp).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorPropertyRows.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorSound.h"
#include "EditorTheme.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "AssetPathPicker.h"
#include "AssetThumbnailLoader.h"
#include "Components.h"
#include "DecalAsset.h"
#include "DecalSystem.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "World.h"

#include "stb_image.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace EditorInternal;

void PropertyLabel(const char* label, const char* tooltip, bool highlighted); // EditorLayer_Inspector.cpp

namespace {

std::string AbsoluteAssetPath(const std::string& p) {
    if (p.empty()) return {};
    return fs::u8path(p).is_absolute() ? p : ProjectPaths::Resolve(p);
}

// .decal files as the editor reads them, re-read when the file changes. The Asset Browser asks
// for every visible tile each frame, so the file's time stamp is checked at most once a second.
struct CachedDecalEntry {
    fs::file_time_type Stamp{};
    std::chrono::steady_clock::time_point Checked{};
    std::shared_ptr<DecalAsset> Asset;
};
std::unordered_map<std::string, CachedDecalEntry>& DecalCache() {
    static std::unordered_map<std::string, CachedDecalEntry> cache;
    return cache;
}
std::shared_ptr<DecalAsset> CachedDecal(const std::string& path, bool force = false) {
    if (path.empty()) return nullptr;
    const std::string abs = AbsoluteAssetPath(path);
    CachedDecalEntry& e = DecalCache()[AssetDatabase::PathKey(abs)];
    const auto now = std::chrono::steady_clock::now();
    if (e.Asset && !force && now - e.Checked < std::chrono::seconds(1)) return e.Asset;
    e.Checked = now;
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::u8path(abs), ec);
    if (!e.Asset || stamp != e.Stamp) {
        e.Stamp = stamp;
        e.Asset = DecalAsset::Load(abs);
    }
    return e.Asset;
}

// The file's own spelling (an Asset Browser key is lower-cased), so a .decal written from it
// reads like the texture's real name.
std::string RealCasePath(const std::string& path) {
    std::error_code ec;
    const fs::path p = fs::canonical(fs::u8path(path), ec);
    return ec ? path : p.generic_u8string();
}

// "White-on-black" masks (most alpha packs) have no alpha channel, or one that's solid.
bool ImageHasRealAlpha(const std::string& path, int& w, int& h) {
    int comp = 0;
    w = h = 0;
    if (!stbi_info(path.c_str(), &w, &h, &comp) || comp < 4) return false;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) return false;
    bool found = false;
    for (size_t i = 3, n = (size_t)w * h * 4; i < n && !found; i += 4) found = px[i] < 250;
    stbi_image_free(px);
    return found;
}

// --- Drawing helpers ------------------------------------------------------------------------

// The albedo thumbnail (tinted, over a dark swatch) with the atlas grid on top. With `frame`,
// hovering a cell highlights it and clicking picks it; the picked frame is outlined. Returns true
// when a frame was clicked.
bool DecalPreview(AssetThumbnailLoader& thumbs, const DecalAsset& a, const glm::vec3& tint, float side,
                  int* frame, float uiScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side);
    ImGui::InvisibleButton("##decalPreview", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(p0, p1, EditorTheme::U32(EditorTheme::Raised), 4.0f * uiScale);

    const std::string albedo = AbsoluteAssetPath(a.AlbedoPath);
    const AssetThumbnailLoader::Thumb t = albedo.empty() ? AssetThumbnailLoader::Thumb{}
                                                         : thumbs.Get(albedo, AssetThumbnailLoader::Source::Image);
    if (t.Tex) {
        const ImVec2 uv0(0.0f, t.FlipV ? 1.0f : 0.0f), uv1(1.0f, t.FlipV ? 0.0f : 1.0f);
        const glm::vec3 c = glm::clamp(a.BaseColor * tint, glm::vec3(0.0f), glm::vec3(1.0f));
        dl->AddImage((ImTextureID)(intptr_t)t.Tex, p0, p1, uv0, uv1,
                     IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 255));
    } else {
        const char* msg = albedo.empty() ? "No albedo" : "Loading...";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (side - ts.x) * 0.5f, p0.y + (side - ts.y) * 0.5f),
                    EditorTheme::U32(EditorTheme::Dim), msg);
    }

    const int cols = std::max(1, a.AtlasCols), rows = std::max(1, a.AtlasRows);
    const float cw = side / cols, ch = side / rows;
    const ImU32 grid = IM_COL32(255, 255, 255, 60);
    for (int i = 1; i < cols; ++i) dl->AddLine(ImVec2(p0.x + cw * i, p0.y), ImVec2(p0.x + cw * i, p1.y), grid);
    for (int i = 1; i < rows; ++i) dl->AddLine(ImVec2(p0.x, p0.y + ch * i), ImVec2(p1.x, p0.y + ch * i), grid);
    dl->AddRect(p0, p1, EditorTheme::U32(EditorTheme::Hairline), 4.0f * uiScale);
    if (!frame || cols * rows <= 1) return false;

    // Frames run left to right, top to bottom (the same order the renderer uses).
    auto cellRect = [&](int f, ImVec2& mn, ImVec2& mx) {
        const int cx = f % cols, cy = f / cols;
        mn = ImVec2(p0.x + cw * cx, p0.y + ch * cy);
        mx = ImVec2(mn.x + cw, mn.y + ch);
    };
    bool picked = false;
    if (hovered) {
        const ImVec2 m = ImGui::GetMousePos();
        const int cx = std::clamp((int)((m.x - p0.x) / cw), 0, cols - 1);
        const int cy = std::clamp((int)((m.y - p0.y) / ch), 0, rows - 1);
        const int f = cy * cols + cx;
        ImVec2 mn, mx;
        cellRect(f, mn, mx);
        dl->AddRectFilled(mn, mx, EditorTheme::U32(EditorTheme::AccentWash));
        EditorUI::SetTooltip("Frame %d - click to use it", f);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && f != *frame) {
            *frame = f;
            picked = true;
        }
    }
    if (*frame >= 0 && *frame < cols * rows) {
        ImVec2 mn, mx;
        cellRect(*frame, mn, mx);
        dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::AccentBright), 0.0f, 0, 2.0f * uiScale);
    }
    return picked;
}

// Two small plots of how strongly the decal lands: by the angle between the surface and the
// projection axis (the asset's angle fade), and by depth through the box (its edge fade).
void FadeCurves(const DecalAsset& a, float width, float uiScale) {
    const float gap = 8.0f * uiScale;
    const float w = std::max(40.0f, (width - gap) * 0.5f), h = 54.0f * uiScale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = EditorTheme::U32(EditorTheme::Accent), axis = EditorTheme::U32(EditorTheme::Hairline),
                label = EditorTheme::U32(EditorTheme::Dim);

    auto plot = [&](const char* id, const char* title, const char* left, const char* right, auto&& fn,
                    const char* tip) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(w, h));
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tip);
        const float th = ImGui::GetTextLineHeight();
        const ImVec2 g0(p0.x, p0.y + th), g1(p0.x + w, p0.y + h - th);
        dl->AddText(p0, label, title);
        dl->AddRect(g0, g1, axis);
        constexpr int kSteps = 48;
        ImVec2 pts[kSteps + 1];
        for (int i = 0; i <= kSteps; ++i) {
            const float u = (float)i / kSteps;
            const float v = std::clamp(fn(u), 0.0f, 1.0f);
            pts[i] = ImVec2(g0.x + u * (g1.x - g0.x), g1.y - v * (g1.y - g0.y));
        }
        dl->AddPolyline(pts, kSteps + 1, line, 0, 1.5f * uiScale);
        dl->AddText(ImVec2(g0.x, g1.y + 1.0f), label, left);
        const ImVec2 rs = ImGui::CalcTextSize(right);
        dl->AddText(ImVec2(g1.x - rs.x, g1.y + 1.0f), label, right);
    };

    // Angle: u = 0 head-on (cos 1) .. 1 edge-on (cos 0). Full strength above the start cosine,
    // gone below the end cosine, smoothstep between (as ModelDecals.glsl does).
    const float cs = std::max(a.AngleFadeStart, a.AngleFadeEnd), ce = std::min(a.AngleFadeStart, a.AngleFadeEnd);
    plot("##fadeAngle", "Angle fade", "0\xC2\xB0", "90\xC2\xB0", [&](float u) {
        const float c = std::cos(u * 1.5707963f);
        if (cs - ce < 1e-4f) return c >= cs ? 1.0f : 0.0f;
        const float t = std::clamp((c - ce) / (cs - ce), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }, "Strength by how squarely the surface faces the projection: head-on at the left, edge-on at\n"
       "the right. Set by the asset's Angle Fade.");
    ImGui::SameLine(0.0f, gap);
    // Depth: u = 0 the far (-Z) face .. 1 the near (+Z) face; Edge Fade is a fraction of the box.
    plot("##fadeDepth", "Depth fade", "back", "front", [&](float u) {
        const float edge = std::max(a.EdgeFade, 1e-4f);
        return (0.5f - std::abs(u - 0.5f)) / edge;
    }, "Strength through the box's depth, back face to front face. Edge Fade softens both ends so\n"
       "the decal doesn't stop with a hard line where its box cuts a curved or uneven surface.");
}

const char* kTierLabels[] = {"512", "1024", "2048", "4096"};
constexpr int kTiers[] = {512, 1024, 2048, 4096};

// Every .decal setting as PropertyRows. Changes are reported on commit (see EditorPropertyRows.h).
void DecalAssetFields(PropertyRows& r, DecalAsset& d, GLFWwindow* owner) {
    if (r.Section(ICON_FA_IMAGE, "Textures", d.AlbedoPath.empty() ? "no albedo" : nullptr, true)) {
        AssetPathPickerOptions img;
        img.Extensions = AssetExts::Images;
        img.DragPayload = "ASSET_TEXTURE_PATH";
        img.DialogFilter = "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0";
        img.Owner = owner;
        r.Path("Albedo", d.AlbedoPath, img, "Colour in rgb, opacity in alpha (or in luminance with Luma Mask on).");
        r.Path("Normal", d.NormalPath, img, "Optional tangent-space normal map (relief, embossed letters, scratches).");
        r.Path("Surface", d.SurfacePath, img, "Optional packed map: r roughness, g metal, b AO, a height.");
        r.Path("Aux", d.AuxPath, img, "Optional single channel: emissive mask, or the peel mask for weathering.");
        r.Check("Luma Mask", d.LumaMask, "Opacity comes from the albedo's brightness: for white-on-black alpha packs\n"
                                         "(most graffiti and grime sheets), coloured by Base Color.");
        r.Check("Invert Mask", d.InvertMask, "Dark marks on a light background (peeling-paint maps): the mask is flipped.");
        int tier = 2;
        for (int i = 0; i < 4; ++i) if (kTiers[i] == d.Tier) tier = i;
        r.Label("Tier", "Slot size in the decal texture arrays. Bigger is sharper up close and costs more of the\n"
                        "256 MB budget: 4096 fills a whole page, 2048 a quarter of one.");
        if (EditorUIPrimitives::Segmented("##tier", &tier, kTierLabels, 4)) {
            d.Tier = kTiers[tier];
            r.MarkChanged();
        }
        r.Int("Atlas Columns", d.AtlasCols, 1, 64, "Frames across, for a sheet of variations (letters, splats).");
        r.Int("Atlas Rows", d.AtlasRows, 1, 64, "Frames down. A placed decal picks one, or lets its Seed pick.");
    }
    if (r.Section(ICON_FA_RULER_COMBINED, "Placement", nullptr, false)) {
        if (r.Vec3("Default Size", d.DefaultSize, 0.01f, "%.2f m",
                   "Width, height and projection depth a newly placed decal starts with.", "WHD"))
            d.DefaultSize = glm::max(d.DefaultSize, glm::vec3(0.001f));
        float startDeg = glm::degrees(std::acos(std::clamp(d.AngleFadeStart, 0.0f, 1.0f)));
        float endDeg = glm::degrees(std::acos(std::clamp(d.AngleFadeEnd, 0.0f, 1.0f)));
        if (r.Float("Angle Fade Start", startDeg, 0.5f, 0.0f, 90.0f, "%.0f deg",
                    "Beyond this angle between the surface and the projection axis the decal starts fading."))
            d.AngleFadeStart = std::cos(glm::radians(startDeg));
        if (r.Float("Angle Fade End", endDeg, 0.5f, 0.0f, 90.0f, "%.0f deg",
                    "...and by this angle it's gone, so it doesn't smear down the sides of a step or a pipe."))
            d.AngleFadeEnd = std::cos(glm::radians(endDeg));
        r.Float("Edge Fade", d.EdgeFade, 0.005f, 0.0f, 0.5f, "%.3f",
                "Softens the front and back of the box (a fraction of its depth).");
        r.Float("Height Blend", d.HeightBlend, 0.01f, 0.0f, 1.0f, "%.2f",
                "Paint sinks into the receiving surface's cracks and cavities (its AO and height) instead of\n"
                "lying evenly over them. 0 = off.");
        r.Float("Height Contrast", d.HeightContrast, 0.05f, 0.0f, 32.0f, "%.2f",
                "How sharply Height Blend separates the high and low parts of the surface.");
    }
    if (r.Section(ICON_FA_TABLE_CELLS, "Channels", nullptr, true)) {
        r.Note("What the decal writes, 0 to 1. A channel at 0 leaves the surface's own value alone.");
        r.Float("Albedo", d.AlbedoOp, 0.01f, 0.0f, 1.0f, "%.2f", "Colour.");
        r.Float("Normal", d.NormalOp, 0.01f, 0.0f, 1.0f, "%.2f", "Relief from the Normal map.");
        r.Float("Roughness", d.RoughnessOp, 0.01f, 0.0f, 1.0f, "%.2f", "Replaces the surface's roughness.");
        r.Float("Metal", d.MetalOp, 0.01f, 0.0f, 1.0f, "%.2f", "Replaces the surface's metalness.");
        r.Float("AO", d.AOOp, 0.01f, 0.0f, 1.0f, "%.2f", "Darkens ambient light (from the Surface map's AO).");
        r.Float("Emissive", d.EmissiveOp, 0.01f, 0.0f, 1.0f, "%.2f", "Glow.");
        // A colour edit (its field or its picker popup) commits once nothing is held any more.
        static bool s_ColorPending = false;
        r.Label("Base Color", "Multiplies the albedo; the whole colour of a Luma Mask decal.");
        s_ColorPending |= EditorUI::ColorEditLinear("##baseColor", &d.BaseColor.x);
        r.Float("Roughness Value", d.Roughness, 0.01f, 0.0f, 1.0f, "%.2f",
                "Roughness written where there's no Surface map.");
        r.Float("Metal Value", d.Metal, 0.01f, 0.0f, 1.0f, "%.2f", "Metalness written where there's no Surface map.");
        r.Label("Emissive Color", "Glow colour (linear, may exceed 1), scaled by the Aux mask when there is one.");
        s_ColorPending |= EditorUI::ColorEditLinear("##emissive", &d.Emissive.x,
                                                    ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        d.Emissive = glm::max(d.Emissive, glm::vec3(0.0f));
        if (s_ColorPending && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
            s_ColorPending = false;
            r.MarkChanged();
        }
    }
    if (r.Section(ICON_FA_STAMP, "Paint", d.Paint ? "on" : nullptr, false)) {
        r.Check("Paint", d.Paint, "Treat the decal as a layer of paint: its own gloss and metal response, raised\n"
                                  "edges, and the weathering below.");
        ImGui::BeginDisabled(!d.Paint);
        r.Float("Paint Roughness", d.PaintRoughness, 0.01f, 0.0f, 1.0f, "%.2f", "Spray paint ~0.35, gloss ~0.15, chalk ~0.9.");
        r.Float("Paint Metal", d.PaintMetal, 0.01f, 0.0f, 1.0f, "%.2f", "Chrome and metallic paints.");
        r.Float("Relief", d.Relief, 0.01f, 0.0f, 1.0f, "%.2f", "How much the paint stands proud of the surface at its edges.");
        r.Check("UV Paint", d.UVPaint, "The emissive channel only glows under UV (black) light.");
        ImGui::EndDisabled();
    }
    if (r.Section(ICON_FA_HOURGLASS_HALF, "Weathering", nullptr, false)) {
        r.Note("This asset's own age, added to each placed decal's and the scene's Decal Grading.");
        r.Float("Fade", d.Fade, 0.01f, 0.0f, 1.0f, "%.2f", "Washes the colour out toward the surface.");
        r.Float("Bleach", d.Bleach, 0.01f, 0.0f, 1.0f, "%.2f", "Sun bleaching: desaturates and lightens.");
        r.Float("Peel", d.Peel, 0.01f, 0.0f, 1.0f, "%.2f", "Flakes off in patches (uses the Aux map when there is one).");
        r.Float("Buff", d.Buff, 0.01f, 0.0f, 1.0f, "%.2f", "A smeared ghost, as if someone tried to scrub it off.");
    }
    if (r.Section(ICON_FA_LAYER_GROUP, "Substrate", d.Substrate.Albedo.empty() ? nullptr : "on", false)) {
        SubstrateDesc& s = d.Substrate;
        r.Note("The material under the paint: where this decal covers, it shows instead of the decal's own\n"
               "colour (concrete under peeling paint). A placed decal on Auto uses its surface's substrate when\n"
               "that has one. Its maps take one 2048 slot of the decal texture budget.");
        AssetPathPickerOptions img;
        img.Extensions = AssetExts::Images;
        img.DragPayload = "ASSET_TEXTURE_PATH";
        img.DialogFilter = "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0";
        img.Owner = owner;
        r.Path("Substrate Albedo", s.Albedo, img, "Colour of the exposed material. Empty = no substrate.");
        r.Path("Substrate Normal", s.Normal, img, "Optional normal map (OpenGL, green up).");
        r.Path("Substrate Roughness", s.Roughness, img, "Optional greyscale roughness, remapped to Rough Min..Max.");
        r.Path("Substrate AO", s.AO, img, "Optional greyscale ambient occlusion.");
        r.Float("Tiling", s.Tiling, 0.01f, 0.05f, 50.0f, "%.2f m", "Metres per repeat, in world space.");
        static bool s_TintPending = false;
        r.Label("Tint", "Multiplies the substrate's colour.");
        s_TintPending |= EditorUI::ColorEditLinear("##subTint", &s.Tint.x);
        if (s_TintPending && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
            s_TintPending = false;
            r.MarkChanged();
        }
        r.Float("Strength", s.Strength, 0.01f, 0.0f, 1.0f, "%.2f", "How fully it replaces the decal's own look.");
        r.Float("Rough Min", s.RoughRange.x, 0.01f, 0.0f, 1.0f, "%.2f", "Roughness where the roughness map is black.");
        r.Float("Rough Max", s.RoughRange.y, 0.01f, 0.0f, 1.0f, "%.2f",
                "Roughness where it's white (everywhere, without a map).");
        r.Check("Mirror", s.Mirror, "Mirror every other repeat, for a sheet that doesn't tile by itself.");
        r.Float("Crop Left", s.Crop.x, 0.005f, 0.0f, 1.0f, "%.3f", "The part of the image that repeats (0..1).");
        r.Float("Crop Top", s.Crop.y, 0.005f, 0.0f, 1.0f, "%.3f", "Down from the image's top edge.");
        r.Float("Crop Right", s.Crop.z, 0.005f, 0.0f, 1.0f, "%.3f", "");
        r.Float("Crop Bottom", s.Crop.w, 0.005f, 0.0f, 1.0f, "%.3f", "");
    }
    if (r.Section(ICON_FA_TAGS, "Library", d.Category.empty() ? nullptr : d.Category.c_str(), false)) {
        r.Text("Category", d.Category, "Where the Decal Library files it, '/'-separated (Graffiti/Letters).");
        r.Text("Tags", d.Tags, "Comma separated search words.");
    }
}

} // namespace

std::string EditorLayer::DecalThumbnailSource(const std::string& decalPath) {
    const auto d = CachedDecal(decalPath);
    return d && !d->Missing && !d->AlbedoPath.empty() ? AbsoluteAssetPath(d->AlbedoPath) : std::string();
}

void EditorLayer::CreateDecalAsset(World& world, AssetLibrary& assets, const std::string& folder,
                                   const std::string& texture) {
    (void)world; // a new file, not a scene edit: nothing to undo in the scene
    (void)assets;
    std::error_code ec;
    const fs::path dir = fs::u8path(folder);
    fs::create_directories(dir, ec);

    DecalAsset d;
    std::string stem = "NewDecal";
    if (!texture.empty()) {
        const std::string tex = RealCasePath(texture);
        stem = fs::u8path(tex).stem().u8string();
        if (stem.rfind("T_", 0) == 0 && stem.size() > 2) stem = stem.substr(2); // T_Tag_Crown -> DA_Tag_Crown
        d.AlbedoPath = ProjectPaths::Relativize(tex);
        int w = 0, h = 0;
        d.LumaMask = !ImageHasRealAlpha(tex, w, h);
        if (w > 0 && h > 0) {
            d.DefaultSize = glm::vec3(1.0f, (float)h / (float)w, 0.25f); // keep the image's aspect
            const int big = std::max(w, h);
            d.Tier = big >= 4096 ? 4096 : big >= 2048 ? 2048 : big >= 1024 ? 1024 : 512;
        }
    }
    fs::path target = dir / ("DA_" + stem + ".decal");
    for (int n = 1; fs::exists(target, ec); ++n) target = dir / ("DA_" + stem + "_" + std::to_string(n) + ".decal");
    d.Path = target.u8string();
    d.Name = target.stem().u8string();
    if (!d.Save()) {
        EditorSound::Play(EditorSound::Event::Blocked);
        Log::Error("Create Decal: couldn't write '" + ProjectPaths::Relativize(d.Path) + "'.");
        return;
    }
    AssetDatabase::EnsureGuid(d.Path);
    m_SelectedAssetKey = AssetDatabase::PathKey(d.Path);
    m_SelectedAssetIsFolder = false;
    EditorSound::Play(EditorSound::Event::Create);
    InvalidateProjectAssetIndex(); // listed in its folder without a manual refresh
    Log::Info("Created decal '" + ProjectPaths::Relativize(d.Path) + "'.");
}

void EditorLayer::DrawDecalAssetEditor(World& world, const std::string& path, bool embedded) {
    // The working copy: re-read when the file changes on disk (another editor, an undo, the cook).
    struct Cache {
        std::string Path;
        fs::file_time_type Stamp{};
        DecalAsset Asset;
    };
    static Cache cache;
    const std::string abs = AbsoluteAssetPath(path);
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::u8path(abs), ec);
    if (abs != cache.Path || stamp != cache.Stamp) {
        cache.Path = abs;
        cache.Stamp = stamp;
        const auto loaded = DecalAsset::Load(abs);
        cache.Asset = *loaded;
    }
    DecalAsset& d = cache.Asset;

    if (!embedded) {
        EditorUIPrimitives::SectionHeader(fs::u8path(abs).filename().u8string().c_str(), ICON_FA_STAMP);
        ImGui::TextDisabled("%s", ProjectPaths::Relativize(abs).c_str());
    }
    if (d.Missing) {
        ImGui::TextColored(EditorUIPrimitives::DangerColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                           "Couldn't read this .decal - see the Console.");
        return;
    }
    if (!embedded) {
        ImGui::BeginDisabled(m_UndoStack.empty());
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_ROTATE_LEFT "  Undo")) m_RequestGlobalUndo = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool duplicate;
        {
            EditorSoundEvents::ScopedClickSilence quiet; // Duplicate / Blocked instead of the click
            duplicate = EditorUIPrimitives::SecondaryButton(ICON_FA_CLONE "  Duplicate");
        }
        if (duplicate) {
            const fs::path base = fs::u8path(abs);
            fs::path target = base.parent_path() / (base.stem().u8string() + "_Copy.decal");
            for (int n = 1; fs::exists(target, ec); ++n)
                target = base.parent_path() / (base.stem().u8string() + "_Copy" + std::to_string(n) + ".decal");
            DecalAsset copy = d;
            copy.Path = target.u8string();
            if (copy.Save()) {
                EditorSound::Play(EditorSound::Event::Duplicate);
                AssetDatabase::EnsureGuid(copy.Path);
                InvalidateProjectAssetIndex();
            } else {
                EditorSound::Play(EditorSound::Event::Blocked);
                Log::Error("Couldn't duplicate '" + ProjectPaths::Relativize(abs) + "'.");
            }
        }
        ImGui::Spacing();
        const float side = std::floor(std::clamp(ImGui::GetContentRegionAvail().x, 64.0f, 220.0f * m_UIScale));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - side) * 0.5f));
        DecalPreview(m_AssetThumbs, d, glm::vec3(1.0f), side, nullptr, m_UIScale);
        ImGui::Spacing();
        FadeCurves(d, ImGui::GetContentRegionAvail().x, m_UIScale);
        ImGui::Spacing();
    }

    bool changed = false;
    PropertyRows rows(150.0f * m_UIScale, changed);
    if (!embedded) rows.Note("Shared by every decal placed from it. Changes save when you let go and show in the viewport.");
    ImGui::PushID("decalAsset");
    DecalAssetFields(rows, d, m_Window);
    ImGui::PopID();
    if (changed) {
        // One undo step per committed edit: the file as it was, written back by Undo.
        std::string before = ReadTextFile(abs);
        d.AtlasCols = std::clamp(d.AtlasCols, 1, 64);
        d.AtlasRows = std::clamp(d.AtlasRows, 1, 64);
        if (d.Save()) {
            PushAssetUndo(world, abs, std::move(before), "Edit Decal", /*assetOnly=*/true);
            cache.Stamp = fs::last_write_time(fs::u8path(abs), ec);
            CachedDecal(abs, /*force=*/true); // tiles and the component extras see it now
        } else {
            EditorSound::Play(EditorSound::Event::Blocked);
            Log::Error("Couldn't save '" + ProjectPaths::Relativize(abs) + "' (read-only?).");
        }
    }
}

void EditorLayer::DrawDecalComponentExtra(World& world, entt::entity entity, ReflectExtraPhase phase) {
    auto* dc = world.Registry.try_get<DecalComponent>(entity);
    if (!dc) return;
    const std::shared_ptr<DecalAsset> asset = CachedDecal(dc->AssetPath);

    if (phase == ReflectExtraPhase::Top) {
        if (dc->AssetPath.empty())
            ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               "No .decal yet - pick one below or drag one from the Asset Browser.");
        else if (!asset || asset->Missing)
            ImGui::TextColored(EditorUIPrimitives::DangerColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               "The .decal is missing or unreadable - nothing is projected.");
        return;
    }

    ImGui::Spacing();
    // Projection box placement.
    {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_MAGNET "  Fit to Surface", ImVec2(half, 0.0f)) &&
            !FitDecalToSurface(world, entity)) {
            EditorSound::Play(EditorSound::Event::Blocked);
            Log::Warn("Fit to Surface: no surface within reach along the decal's -Z.");
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Move onto the surface the box projects at (along -Z) and turn to face out of it,\n"
                                 "keeping its spin. Use it after moving a decal by hand.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!asset || asset->Missing);
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_EXPAND "  Asset Size", ImVec2(-FLT_MIN, 0.0f)) && asset) {
            PushUndo(world, "Reset Decal Size");
            dc->Size = asset->DefaultSize;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            EditorUI::SetTooltip("Reset Size to the asset's Default Size.");
    }
    if (!asset || asset->Missing) return;

    // Preview + the live atlas-frame picker.
    ImGui::Spacing();
    const int frames = std::max(1, asset->AtlasCols) * std::max(1, asset->AtlasRows);
    const float side = std::floor(std::clamp(ImGui::GetContentRegionAvail().x * 0.6f, 64.0f, 180.0f * m_UIScale));
    ImGui::BeginGroup();
    int frame = dc->Frame;
    if (DecalPreview(m_AssetThumbs, *asset, dc->Tint, side, &frame, m_UIScale)) {
        PushUndo(world, "Pick Decal Frame");
        dc->Frame = frame;
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(asset->Name.c_str());
    ImGui::TextDisabled("%d px  |  %s", asset->Tier, asset->LumaMask ? "luma mask" : "alpha");
    if (frames > 1) {
        ImGui::TextDisabled("%d x %d frames", asset->AtlasCols, asset->AtlasRows);
        if (dc->Frame < 0) ImGui::TextDisabled("Frame: from Seed (%d)", DecalSystem::FrameFor(dc->Seed, frames));
        else ImGui::TextDisabled("Frame: %d", dc->Frame);
        ImGui::BeginDisabled(dc->Frame < 0);
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_DICE "  From Seed")) {
            PushUndo(world, "Pick Decal Frame");
            dc->Frame = -1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            EditorUI::SetTooltip("Let the Seed pick the frame (Frame -1), so copies vary on their own.");
    }
    if (asset->Paint) ImGui::TextDisabled(ICON_FA_STAMP "  paint%s", asset->UVPaint ? ", UV" : "");
    ImGui::EndGroup();
    if (frames > 1 && ImGui::IsItemHovered()) EditorUI::SetTooltip("Click a frame on the left to use it.");

    // Channel matrix: what the asset writes x this decal's multiplier x Opacity = what lands.
    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Channels", ICON_FA_TABLE_CELLS);
    struct Row { const char* Name; float Asset; float* Mine; const char* Tip; };
    const Row rows[] = {
        {"Albedo", asset->AlbedoOp, &dc->AlbedoOp, "Colour"},
        {"Normal", asset->NormalPath.empty() ? 0.0f : asset->NormalOp, &dc->NormalOp, "Relief (needs the asset's Normal map)"},
        {"Roughness", asset->RoughnessOp, &dc->RoughnessOp, "Roughness"},
        {"Metal", asset->MetalOp, &dc->MetalOp, "Metalness"},
        {"AO", asset->AOOp, &dc->AOOp, "Ambient occlusion"},
        {"Emissive", asset->EmissiveOp, &dc->EmissiveOp, "Glow"},
    };
    if (ImGui::BeginTable("##decalChannels", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("This", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (const Row& row : rows) {
            ImGui::PushID(row.Name);
            const float result = std::clamp(row.Asset * *row.Mine * dc->Opacity, 0.0f, 1.0f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.Name);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", row.Tip);
            ImGui::TableNextColumn();
            if (row.Asset <= 0.0f) ImGui::TextDisabled("off");
            else ImGui::Text("%.2f", row.Asset);
            ImGui::TableNextColumn();
            ImGui::Text("x%.2f", *row.Mine);
            ImGui::TableNextColumn();
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight() * 0.55f;
                const float y = p0.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + w, y + h), EditorTheme::U32(EditorTheme::Field), 2.0f);
                if (result > 0.0f)
                    dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + w * result, y + h),
                                      EditorTheme::U32(EditorTheme::Accent), 2.0f);
                ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("%.2f asset x %.2f this decal x %.2f Opacity = %.2f", row.Asset, *row.Mine,
                                         dc->Opacity, result);
            }
            ImGui::TableNextColumn();
            // Quick on/off for this decal's multiplier (the slider above sets any value).
            bool on = *row.Mine > 0.0f;
            ImGui::BeginDisabled(row.Asset <= 0.0f);
            if (EditorUIPrimitives::Checkbox("##on", &on)) {
                PushUndo(world, on ? "Enable Decal Channel" : "Disable Decal Channel");
                *row.Mine = on ? 1.0f : 0.0f;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                EditorUI::SetTooltip(row.Asset <= 0.0f ? "The asset doesn't write this channel." : "This decal writes this channel.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (asset->HeightBlend > 0.0f)
        ImGui::TextDisabled("Height blend %.2f: sinks into the surface's cracks.", asset->HeightBlend);

    ImGui::Spacing();
    FadeCurves(*asset, ImGui::GetContentRegionAvail().x, m_UIScale);

    // The asset itself, editable in place (it's shared by every decal placed from it).
    ImGui::Spacing();
    const std::string title = std::string(ICON_FA_STAMP "  Edit ") + asset->Name + "###decalAssetInline";
    if (EditorUIPrimitives::Foldout(title.c_str())) {
        ImGui::TextDisabled("Shared: changes apply to every decal using %s.", asset->Name.c_str());
        DrawDecalAssetEditor(world, dc->AssetPath, /*embedded=*/true);
    }
}
