#include "AssetImporterInspector.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include <imgui.h>
#include <string>

namespace {

// Local equivalent of EditorLayer.cpp's PropertyLabel/AlignToColumn (that one lives in an
// anonymous namespace there, so it isn't reachable from this translation unit) — same fixed-
// column layout convention, kept in sync by eye rather than shared, since duplicating one small
// free function is cheaper than exporting it across a module boundary for a single reuse site.
constexpr float kColumnWidth = 170.0f;

void Row(const char* label, const char* tooltip = nullptr) {
    float lineStartX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tooltip);
    ImGui::SameLine(lineStartX + kColumnWidth);
}

} // namespace

void AssetImporterInspector::DrawTextureSettings(TextureImportSettings& settings, bool& isDirty) {
    ImGui::PushItemWidth(-1.0f);

    // #184 — the old tooltip called this "mainly documentation"; Texture::UploadFromFile has
    // enforced it since #198, so say what each type actually does, and grey out the toggles it
    // overrides below instead of letting them look editable.
    Row("Texture Type", "Default: colour or data texture, every setting below applies as shown.\n"
                        "Normal Map: always uploaded linear (sRGB forced off).\n"
                        "Sprite / 2D: always Clamp to Edge with no mipmaps, for UI and sprite atlases.");
    const char* kTypes[] = {"Default", "Normal Map", "Sprite / 2D"};
    int typeIdx = (int)settings.TextureType;
    if (ImGui::Combo("##TexType", &typeIdx, kTypes, IM_ARRAYSIZE(kTypes))) {
        settings.TextureType = (TextureImportSettings::Type)typeIdx;
        // A normal/data map is never color data - flipping sRGB off automatically saves a click
        // and avoids the single most common Unity-importer mistake this toggle exists to prevent.
        if (settings.TextureType == TextureImportSettings::Type::NormalMap) settings.IsSRGB = false;
        isDirty = true;
    }

    const bool isSprite = settings.TextureType == TextureImportSettings::Type::Sprite2D;
    const bool isNormal = settings.TextureType == TextureImportSettings::Type::NormalMap;

    Row("Generate Mipmaps", "Builds progressively smaller versions for filtering at a\ndistance. Off saves memory/import time for UI or other never-minified textures.");
    ImGui::BeginDisabled(isSprite);
    isDirty |= EditorUIPrimitives::Checkbox("##Mipmaps", &settings.GenerateMipmaps);
    ImGui::EndDisabled();
    if (isSprite && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        EditorUI::SetTooltip("Sprite / 2D textures never get mipmaps.");

    Row("sRGB (Color Texture)", "On for albedo/base-color textures (authored in sRGB by every\npaint/photo tool). Off for normal maps and other data maps (roughness,\nmetallic, AO, height) - those numbers ARE linear already.");
    ImGui::BeginDisabled(isNormal);
    isDirty |= EditorUIPrimitives::Checkbox("##sRGB", &settings.IsSRGB);
    ImGui::EndDisabled();
    if (isNormal && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        EditorUI::SetTooltip("Normal maps are always linear.");

    Row("Filter Mode", "Point = blocky/pixel-art. Bilinear = smooth, no blending between\nmip levels. Trilinear = smooth with blending between mip levels too\n(best quality at oblique angles, marginally more GPU cost).");
    const char* kFilters[] = {"Point", "Bilinear", "Trilinear"};
    int filterIdx = (int)settings.FilterMode;
    if (ImGui::Combo("##Filter", &filterIdx, kFilters, IM_ARRAYSIZE(kFilters))) {
        settings.FilterMode = (TextureImportSettings::Filter)filterIdx;
        isDirty = true;
    }

    Row("Aniso Level", "Anisotropic filtering: keeps a texture sharp when seen at a grazing\nangle (floors, roads, walls receding into the distance). 1 = off. Needs\nmipmaps and a Bilinear/Trilinear filter; capped at what the GPU supports.");
    {
        int aniso = settings.AnisoLevel;
        if (ImGui::SliderInt("##Aniso", &aniso, 1, 16)) {
            settings.AnisoLevel = aniso;
            isDirty = true;
        }
    }

    Row("Wrap Mode", "Repeat tiles past 0..1 UV (most surface textures). Clamp to Edge\nsmears the edge pixel instead - for a texture that should never visibly\ntile (a decal, a UI sprite).");
    const char* kWraps[] = {"Repeat", "Clamp to Edge"};
    int wrapIdx = (int)settings.WrapMode;
    ImGui::BeginDisabled(isSprite);
    if (ImGui::Combo("##Wrap", &wrapIdx, kWraps, IM_ARRAYSIZE(kWraps))) {
        settings.WrapMode = (TextureImportSettings::Wrap)wrapIdx;
        isDirty = true;
    }
    ImGui::EndDisabled();
    if (isSprite && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        EditorUI::SetTooltip("Sprite / 2D textures always clamp to edge.");

    Row("Max Size", "Downscales on import if the source exceeds this in either\ndimension (aspect ratio preserved). Lower this for a texture that's\nauthored much larger than it'll ever be seen at.");
    // #184 — 0 (no limit) and 8192 are valid values Texture honours; they used to be missing, and
    // any value outside the list displayed as "2048". An off-list value now shows as itself.
    const int kSizes[] = {256, 512, 1024, 2048, 4096, 8192, 0};
    const char* kSizeLabels[] = {"256", "512", "1024", "2048", "4096", "8192", "No limit"};
    int sizeIdx = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i) if (kSizes[i] == settings.MaxTextureSize) sizeIdx = i;
    const std::string customLabel = std::to_string(settings.MaxTextureSize);
    if (ImGui::BeginCombo("##MaxSize", sizeIdx >= 0 ? kSizeLabels[sizeIdx] : customLabel.c_str())) {
        for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i) {
            if (ImGui::Selectable(kSizeLabels[i], i == sizeIdx) && i != sizeIdx) {
                settings.MaxTextureSize = kSizes[i];
                isDirty = true;
            }
        }
        ImGui::EndCombo();
    }

    Row("Compression", "GPU block compression (BCn), encoded once on import and cached.\n"
                       "None: raw 8-bit, largest.\n"
                       "Normal: BC1 (opaque colour, 8x smaller than RGBA8) or BC3 (with alpha, 4x);\n"
                       "BC4 / BC5 for one- and two-channel data maps.\n"
                       "High Quality: same formats, slower encode with fewer colour artifacts.");
    const char* kCompression[] = {"None", "Normal", "High Quality"};
    int compIdx = (int)settings.CompressionMode;
    if (ImGui::Combo("##Compression", &compIdx, kCompression, IM_ARRAYSIZE(kCompression))) {
        settings.CompressionMode = (TextureImportSettings::Compression)compIdx;
        isDirty = true;
    }

    ImGui::PopItemWidth();
}

void AssetImporterInspector::DrawModelSettings(ModelImportSettings& settings, bool& isDirty,
                                               const std::vector<std::pair<std::string, float>>* clips) {
    ImGui::PushItemWidth(-1.0f);

    Row("Import Scale", "Multiplies whatever unit-scale correction Assimp already derives\nfrom the file's own metadata (FBX/glTF commonly embed one; plain .obj\nusually doesn't). 1.0 = no extra correction.");
    isDirty |= ImGui::DragFloat("##Scale", &settings.GlobalScale, 0.01f, 0.001f, 1000.0f, "%.3f");

    Row("Generate Normals/Tangents", "Computes smooth normals and tangent-space basis\nvectors for normal mapping. Leave on unless the source file already\nauthors its own normals you specifically want preserved as-is.");
    isDirty |= EditorUIPrimitives::Checkbox("##Normals", &settings.ImportNormals);

    Row("Optimize Mesh", "Merges identical/duplicate vertices and welds mesh pieces that\nshare a material - fewer draw calls and less GPU memory, purely a\nperformance optimization with no visible difference.");
    isDirty |= EditorUIPrimitives::Checkbox("##OptimizeGraph", &settings.OptimizeGraph);

    ImGui::Spacing();
    ImGui::SeparatorText("Rig & Animation");

    Row("Import Animation", "Reads animation clips embedded in the file. Off if this asset\nis only ever used as a static prop - skips clip data entirely.");
    isDirty |= EditorUIPrimitives::Checkbox("##ImportAnim", &settings.ImportAnimations);

    Row("Import Skeleton", "Reads bone weights so the mesh can be posed/animated. Off\nimports every mesh as static geometry, baked at its bind pose -\ncheaper, but Import Animation has nothing to play without this.");
    isDirty |= EditorUIPrimitives::Checkbox("##ImportSkeleton", &settings.ImportSkeleton);

    // Clip trims: cut a clip to a range of its take.
    Row("Clip Trims", "Cut a clip to a range (seconds) of the source take: only that range plays, and it is the\nclip's length everywhere (loops, blend trees, root motion, the Animator's analysis).\nEnd 0 = to the end of the take. Apply to reimport.");
    {
        int remove = -1;
        for (int i = 0; i < (int)settings.ClipTrims.size(); ++i) {
            auto& t = settings.ClipTrims[i];
            ImGui::PushID(i);
            char len[32] = "";
            if (clips)
                for (const auto& c : *clips)
                    if (c.first == t.Clip) std::snprintf(len, sizeof len, " (%.2f s)", c.second);
            ImGui::TextUnformatted((t.Clip + len).c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            isDirty |= ImGui::DragFloat("##ts", &t.StartSeconds, 0.01f, 0.0f, 10000.0f, "start %.2f s");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            isDirty |= ImGui::DragFloat("##te", &t.EndSeconds, 0.01f, 0.0f, 10000.0f, "end %.2f s");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK)) remove = i;
            ImGui::PopID();
        }
        if (remove >= 0) { settings.ClipTrims.erase(settings.ClipTrims.begin() + remove); isDirty = true; }
        if (ImGui::BeginCombo("##addtrim", "+ Trim a clip...")) {
            if (clips)
                for (const auto& c : *clips) {
                    bool have = false;
                    for (const auto& t : settings.ClipTrims) have |= t.Clip == c.first;
                    if (!have && ImGui::Selectable(c.first.c_str())) {
                        settings.ClipTrims.push_back({c.first, 0.0f, c.second});
                        isDirty = true;
                    }
                }
            else ImGui::TextDisabled("Select the model in the Asset Browser to list its clips.");
            ImGui::EndCombo();
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Materials");

    Row("Material Import Mode", "Import Embedded reads the file's own materials/textures\n(today's default). Create Synthetic ignores them and assigns one neutral\nPBR material to author from scratch. None leaves every mesh's material\nat bare defaults (flat white, non-metal, mid roughness).");
    const char* kMatModes[] = {"Import Embedded", "Create Synthetic Materials", "None"};
    int matIdx = (int)settings.MaterialImportMode;
    if (ImGui::Combo("##MatMode", &matIdx, kMatModes, IM_ARRAYSIZE(kMatModes))) {
        settings.MaterialImportMode = (ModelImportSettings::MaterialMode)matIdx;
        isDirty = true;
    }

    ImGui::PopItemWidth();
}

void AssetImporterInspector::DrawApplyRevertFooter(bool isDirty, const std::function<void()>& onApply,
    const std::function<void()>& onRevert) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!isDirty) ImGui::BeginDisabled();
    float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Revert", ImVec2(halfWidth, 0.0f)) && onRevert) onRevert();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Discard these changes, reloading the settings\ncurrently saved for this asset.");
    ImGui::SameLine();
    if (ImGui::Button("Apply", ImVec2(halfWidth, 0.0f)) && onApply) onApply();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Save these settings and re-import the asset\nwith them right now.");
    if (!isDirty) ImGui::EndDisabled();
}
