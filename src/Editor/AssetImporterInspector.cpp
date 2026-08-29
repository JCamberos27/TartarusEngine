#include "AssetImporterInspector.h"
#include "EditorUIHelpers.h"
#include <imgui.h>

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

    Row("Texture Type", "What this texture is used for - mainly documentation today, but\nsRGB should only ever be on for Default/Sprite, never Normal Map.");
    const char* kTypes[] = {"Default", "Normal Map", "Sprite / 2D", "Cubemap"};
    int typeIdx = (int)settings.TextureType;
    if (ImGui::Combo("##TexType", &typeIdx, kTypes, IM_ARRAYSIZE(kTypes))) {
        settings.TextureType = (TextureImportSettings::Type)typeIdx;
        // A normal/data map is never color data - flipping sRGB off automatically saves a click
        // and avoids the single most common Unity-importer mistake this toggle exists to prevent.
        if (settings.TextureType == TextureImportSettings::Type::NormalMap) settings.IsSRGB = false;
        isDirty = true;
    }

    Row("Generate Mipmaps", "Builds progressively smaller versions for filtering at a\ndistance. Off saves memory/import time for UI or other never-minified textures.");
    isDirty |= ImGui::Checkbox("##Mipmaps", &settings.GenerateMipmaps);

    Row("sRGB (Color Texture)", "On for albedo/base-color textures (authored in sRGB by every\npaint/photo tool). Off for normal maps and other data maps (roughness,\nmetallic, AO, height) - those numbers ARE linear already.");
    isDirty |= ImGui::Checkbox("##sRGB", &settings.IsSRGB);

    Row("Is Readable", "Kept for parity with Unity's importer and saved with the rest of\nthese settings, but nothing in this engine reads pixel data back off the\nGPU today - this flag has no effect yet.");
    isDirty |= ImGui::Checkbox("##Readable", &settings.IsReadable);

    Row("Filter Mode", "Point = blocky/pixel-art. Bilinear = smooth, no blending between\nmip levels. Trilinear = smooth with blending between mip levels too\n(best quality at oblique angles, marginally more GPU cost).");
    const char* kFilters[] = {"Point", "Bilinear", "Trilinear"};
    int filterIdx = (int)settings.FilterMode;
    if (ImGui::Combo("##Filter", &filterIdx, kFilters, IM_ARRAYSIZE(kFilters))) {
        settings.FilterMode = (TextureImportSettings::Filter)filterIdx;
        isDirty = true;
    }

    Row("Wrap Mode", "Repeat tiles past 0..1 UV (most surface textures). Clamp to Edge\nsmears the edge pixel instead - for a texture that should never visibly\ntile (a decal, a UI sprite).");
    const char* kWraps[] = {"Repeat", "Clamp to Edge"};
    int wrapIdx = (int)settings.WrapMode;
    if (ImGui::Combo("##Wrap", &wrapIdx, kWraps, IM_ARRAYSIZE(kWraps))) {
        settings.WrapMode = (TextureImportSettings::Wrap)wrapIdx;
        isDirty = true;
    }

    Row("Max Size", "Downscales on import if the source exceeds this in either\ndimension (aspect ratio preserved). Lower this for a texture that's\nauthored much larger than it'll ever be seen at.");
    const int kSizes[] = {512, 1024, 2048, 4096};
    const char* kSizeLabels[] = {"512", "1024", "2048", "4096"};
    int sizeIdx = 2;
    for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i) if (kSizes[i] == settings.MaxTextureSize) sizeIdx = i;
    if (ImGui::Combo("##MaxSize", &sizeIdx, kSizeLabels, IM_ARRAYSIZE(kSizeLabels))) {
        settings.MaxTextureSize = kSizes[sizeIdx];
        isDirty = true;
    }

    ImGui::PopItemWidth();
}

void AssetImporterInspector::DrawModelSettings(ModelImportSettings& settings, bool& isDirty) {
    ImGui::PushItemWidth(-1.0f);

    Row("Import Scale", "Multiplies whatever unit-scale correction Assimp already derives\nfrom the file's own metadata (FBX/glTF commonly embed one; plain .obj\nusually doesn't). 1.0 = no extra correction.");
    isDirty |= ImGui::DragFloat("##Scale", &settings.GlobalScale, 0.01f, 0.001f, 1000.0f, "%.3f");

    Row("Generate Normals/Tangents", "Computes smooth normals and tangent-space basis\nvectors for normal mapping. Leave on unless the source file already\nauthors its own normals you specifically want preserved as-is.");
    isDirty |= ImGui::Checkbox("##Normals", &settings.ImportNormals);

    Row("Optimize Mesh", "Merges identical/duplicate vertices and welds mesh pieces that\nshare a material - fewer draw calls and less GPU memory, purely a\nperformance optimization with no visible difference.");
    isDirty |= ImGui::Checkbox("##OptimizeGraph", &settings.OptimizeGraph);

    ImGui::Spacing();
    ImGui::SeparatorText("Rig & Animation");

    Row("Import Animation", "Reads animation clips embedded in the file. Off if this asset\nis only ever used as a static prop - skips clip data entirely.");
    isDirty |= ImGui::Checkbox("##ImportAnim", &settings.ImportAnimations);

    Row("Import Skeleton", "Reads bone weights so the mesh can be posed/animated. Off\nimports every mesh as static geometry, baked at its bind pose -\ncheaper, but Import Animation has nothing to play without this.");
    isDirty |= ImGui::Checkbox("##ImportSkeleton", &settings.ImportSkeleton);

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
