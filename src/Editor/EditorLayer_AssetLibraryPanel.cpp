// The Asset Library panel: browse an asset collection that lives outside the project (a folder
// of packs laid out <Category>/<Asset>/, like a purchased-asset library) and import from it
// without leaving the editor. Picking an asset shows what it holds and a preview; Import brings
// the whole asset folder in through the same path as dropping it on the window (copied into
// project/assets/ with its layout, textures matched, materials extracted).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "AssetImport.h"
#include "AssetLibrary.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "FileDialog.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "Texture.h"

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace EditorInternal;

namespace {

struct DirEntry {
    std::string Name;
    std::string Path;
    bool HasSubdirs = false;
};

// What an asset folder holds, gathered once when it's selected.
struct FolderSummary {
    std::string Path;
    int Models = 0, Textures = 0, Sounds = 0, Other = 0, Skipped = 0, Subfolders = 0;
    uintmax_t Bytes = 0;
    std::vector<std::string> ModelFiles; // relative to the folder
    std::string PreviewImage;            // absolute, "" if none
    bool Truncated = false;
};

struct LibraryState {
    std::string Root;                                   // the library folder these caches are for
    std::map<std::string, std::vector<DirEntry>> Children;
    std::vector<std::string> Index;                     // every folder, for search (built lazily)
    bool IndexBuilt = false;
    std::string Selected;
    FolderSummary Summary;
    std::map<std::string, std::shared_ptr<Texture>> Previews;
    char Search[128] = "";
};

LibraryState& State() {
    static LibraryState s;
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

const std::vector<DirEntry>& ChildrenOf(const std::string& dir) {
    LibraryState& st = State();
    auto it = st.Children.find(dir);
    if (it != st.Children.end()) return it->second;
    std::vector<DirEntry> out;
    std::error_code ec;
    for (fs::directory_iterator d(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && d != end; d.increment(ec)) {
        std::error_code fec;
        if (!d->is_directory(fec)) continue;
        const std::string name = d->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        DirEntry e{name, d->path().string(), false};
        for (fs::directory_iterator sub(d->path(), fs::directory_options::skip_permission_denied, fec), send; !fec && sub != send; sub.increment(fec)) {
            std::error_code sec;
            if (sub->is_directory(sec)) { e.HasSubdirs = true; break; }
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return Lower(a.Name) < Lower(b.Name); });
    return st.Children[dir] = std::move(out);
}

void BuildIndex() {
    LibraryState& st = State();
    st.Index.clear();
    std::error_code ec;
    size_t visited = 0;
    fs::recursive_directory_iterator walk(st.Root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && walk != end && visited < 50000; walk.increment(ec), ++visited) {
        std::error_code fec;
        if (!walk->is_directory(fec)) continue;
        if (walk.depth() >= 4) walk.disable_recursion_pending();
        st.Index.push_back(walk->path().string());
    }
    st.IndexBuilt = true;
}

bool IsPreviewFolder(const fs::path& p) {
    const std::string n = Lower(p.filename().string());
    return n == "preview" || n == "previews" || n == "render" || n == "renders" || n == "screenshot" ||
           n == "screenshots" || n == "thumbnail" || n == "thumbnails";
}

FolderSummary Summarize(const std::string& dir) {
    FolderSummary s;
    s.Path = dir;
    std::string albedoCandidate, anyImage;
    std::error_code ec;
    size_t visited = 0;
    for (fs::recursive_directory_iterator walk(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && walk != end; walk.increment(ec)) {
        if (++visited > 20000) { s.Truncated = true; break; }
        std::error_code fec;
        if (walk->is_directory(fec)) { if (walk.depth() == 0) ++s.Subfolders; continue; }
        if (!walk->is_regular_file(fec)) continue;
        const std::string path = walk->path().string();
        s.Bytes += walk->file_size(fec);
        if (AssetImport::IsSourceOnlyFile(path)) { ++s.Skipped; continue; }
        const std::string kind = AssetImport::ImportKind(path);
        if (kind == "model") {
            ++s.Models;
            s.ModelFiles.push_back(walk->path().lexically_relative(dir).generic_string());
        } else if (kind == "texture") {
            ++s.Textures;
            const std::string name = Lower(walk->path().stem().string());
            const bool inPreview = IsPreviewFolder(walk->path().parent_path());
            if (s.PreviewImage.empty() && (inPreview || name.find("preview") != std::string::npos)) s.PreviewImage = path;
            if (albedoCandidate.empty() && AssetImport::GuessTextureKind(path) == AssetImport::TextureKind::Color &&
                (name.find("albedo") != std::string::npos || name.find("basecolor") != std::string::npos ||
                 name.find("base_color") != std::string::npos || name.find("diffuse") != std::string::npos))
                albedoCandidate = path;
            if (anyImage.empty()) anyImage = path;
        } else if (kind == "sound") {
            ++s.Sounds;
        } else {
            ++s.Other;
        }
    }
    if (s.PreviewImage.empty()) s.PreviewImage = !albedoCandidate.empty() ? albedoCandidate : anyImage;
    std::sort(s.ModelFiles.begin(), s.ModelFiles.end());
    return s;
}

std::shared_ptr<Texture> PreviewTexture(const std::string& path) {
    LibraryState& st = State();
    auto it = st.Previews.find(path);
    if (it != st.Previews.end()) return it->second;
    if (st.Previews.size() > 32) st.Previews.clear();
    TextureImportSettings settings; // small and quick: a preview, not an asset
    settings.MaxTextureSize = 512;
    settings.GenerateMipmaps = false;
    auto tex = std::make_shared<Texture>(path, settings);
    if (!tex->IsValid()) tex = nullptr;
    return st.Previews[path] = tex;
}

std::string FormatBytes(uintmax_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", b / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", b / double(1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    return buf;
}

} // namespace

void EditorLayer::DrawAssetLibraryPanel(World& world, AssetLibrary& assets) {
    EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.ShowAssetLibrary) return;
    ImGui::SetNextWindowSize(ImVec2(620.0f * m_UIScale, 420.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    bool open = true;
    const bool visible = ImGui::Begin(ICON_FA_BOX_ARCHIVE "  Asset Library", &open);
    PopTabChromeText();
    if (!open) { prefs.ShowAssetLibrary = false; EditorSettings::Save(); }
    if (!visible) { ImGui::End(); return; }

    LibraryState& st = State();
    auto chooseFolder = [&]() {
        const std::string picked = FileDialog::PickFolder(m_Window);
        if (picked.empty()) return;
        prefs.AssetLibraryPath = picked;
        EditorSettings::Save();
    };

    std::error_code ec;
    if (prefs.AssetLibraryPath.empty() || !fs::is_directory(prefs.AssetLibraryPath, ec)) {
        ImGui::Spacing();
        ImGui::TextWrapped(prefs.AssetLibraryPath.empty()
            ? "Point this panel at a folder of assets outside the project - a library of packs laid out "
              "<Category>/<Asset>/ - to browse it and import assets with one click."
            : "The library folder can't be found:");
        if (!prefs.AssetLibraryPath.empty()) ImGui::TextDisabled("%s", prefs.AssetLibraryPath.c_str());
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Choose Library Folder...")) chooseFolder();
        ImGui::End();
        return;
    }
    if (st.Root != prefs.AssetLibraryPath) { // a new library: drop every cache
        const std::string root = prefs.AssetLibraryPath;
        st = LibraryState{};
        st.Root = root;
    }

    // Toolbar: library name, search, refresh, change folder.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ICON_FA_BOX_ARCHIVE);
    ImGui::SameLine();
    ImGui::TextUnformatted(fs::path(st.Root).filename().string().c_str());
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", st.Root.c_str());
    ImGui::SameLine();
    const float buttons = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - buttons));
    if (ImGui::InputTextWithHint("##libsearch", ICON_FA_MAGNIFYING_GLASS "  Search assets...", st.Search, sizeof(st.Search)) &&
        st.Search[0] && !st.IndexBuilt)
        BuildIndex();
    ImGui::SameLine();
    if (ActionButton(ICON_FA_ROTATE, "Rescan the library folder")) {
        const std::string root = st.Root, selected = st.Selected;
        st = LibraryState{};
        st.Root = root;
        st.Selected = selected;
    }
    ImGui::SameLine();
    if (ActionButton(ICON_FA_FOLDER_OPEN, "Choose a different library folder")) chooseFolder();

    if (ImGui::BeginTable("##assetlib", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow();

        // Left: the folder tree, or search results.
        ImGui::TableNextColumn();
        ImGui::BeginChild("##libtree", ImVec2(0, 0));
        auto select = [&](const std::string& path) {
            if (st.Selected == path) return;
            st.Selected = path;
            st.Summary = Summarize(path);
        };
        if (st.Search[0]) {
            const std::string needle = Lower(st.Search);
            int shown = 0;
            for (const std::string& p : st.Index) {
                if (Lower(fs::path(p).filename().string()).find(needle) == std::string::npos) continue;
                const std::string rel = fs::path(p).lexically_relative(st.Root).generic_string();
                if (ImGui::Selectable((ICON_FA_FOLDER "  " + rel + "##" + p).c_str(), st.Selected == p)) select(p);
                if (++shown >= 300) { ImGui::TextDisabled("More matches - refine the search."); break; }
            }
            if (shown == 0) ImGui::TextDisabled("No folder matches '%s'.", st.Search);
        } else {
            std::function<void(const std::string&)> drawDir = [&](const std::string& dir) {
                for (const DirEntry& e : ChildrenOf(dir)) {
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (!e.HasSubdirs) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (st.Selected == e.Path) flags |= ImGuiTreeNodeFlags_Selected;
                    const bool opened = ImGui::TreeNodeEx(e.Path.c_str(), flags, "%s  %s", e.HasSubdirs ? ICON_FA_FOLDER : ICON_FA_CUBE, e.Name.c_str());
                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(e.Path);
                    if (opened && e.HasSubdirs) {
                        drawDir(e.Path);
                        ImGui::TreePop();
                    }
                }
            };
            drawDir(st.Root);
        }
        ImGui::EndChild();

        // Right: what the selected folder holds, a preview and Import.
        ImGui::TableNextColumn();
        ImGui::BeginChild("##libdetail", ImVec2(0, 0));
        if (st.Selected.empty() || !fs::is_directory(st.Selected, ec)) {
            ImGui::TextDisabled("Select an asset folder on the left.");
        } else {
            const FolderSummary& s = st.Summary;
            const std::string name = fs::path(s.Path).filename().string();
            ImGui::TextUnformatted(name.c_str());
            ImGui::TextDisabled("%s", fs::path(s.Path).lexically_relative(st.Root).generic_string().c_str());
            ImGui::Spacing();

            if (!s.PreviewImage.empty()) {
                if (auto tex = PreviewTexture(s.PreviewImage)) {
                    const float maxW = std::min(ImGui::GetContentRegionAvail().x, 260.0f * m_UIScale);
                    const float aspect = tex->Height() > 0 ? (float)tex->Width() / (float)tex->Height() : 1.0f;
                    const ImVec2 size = aspect >= 1.0f ? ImVec2(maxW, maxW / aspect) : ImVec2(maxW * aspect, maxW);
                    ImGui::Image((ImTextureID)(intptr_t)tex->GLHandle(), size);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", fs::path(s.PreviewImage).filename().string().c_str());
                }
            }

            std::string counts;
            auto add = [&](int n, const char* what) {
                if (n <= 0) return;
                if (!counts.empty()) counts += ", ";
                counts += std::to_string(n) + " " + what + (n == 1 ? "" : "s");
            };
            add(s.Models, "model"); add(s.Textures, "texture"); add(s.Sounds, "sound");
            ImGui::Text("%s%s", counts.empty() ? "Nothing importable" : counts.c_str(), s.Truncated ? " (partial - very large folder)" : "");
            ImGui::TextDisabled("%s on disk%s", FormatBytes(s.Bytes).c_str(),
                                s.Skipped ? (", " + std::to_string(s.Skipped) + " source file(s) not imported").c_str() : "");

            const fs::path inProject = fs::path(ProjectPaths::Resolve("assets")) / name;
            const bool alreadyIn = fs::exists(inProject, ec);
            if (alreadyIn) {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), ICON_FA_CHECK "  In the project as %s",
                                   ProjectPaths::Relativize(inProject.string()).c_str());
            }

            ImGui::Spacing();
            const bool importable = s.Models + s.Textures + s.Sounds > 0;
            ImGui::BeginDisabled(!importable || !m_EditorCameraPtr);
            const std::string label = std::string(ICON_FA_FILE_IMPORT "  ") + (alreadyIn ? "Import Again" : "Import") + " '" + name + "'";
            if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f)) && m_EditorCameraPtr)
                HandleDroppedFiles(world, assets, *m_EditorCameraPtr, true, {s.Path});
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                EditorUI::SetTooltip(importable
                    ? "Copies the whole folder into the project's assets folder, keeping its layout,\n"
                      "then imports its models, textures and sounds. Models get their textures\n"
                      "matched and editable materials created."
                    : "This folder has no models, textures or sounds of its own.\nPick an asset inside it.");
            if (alreadyIn) ImGui::TextDisabled("Importing again adds a second copy.");

            if (!s.ModelFiles.empty()) {
                ImGui::Spacing();
                ImGui::SeparatorText("Models");
                ImGui::TextDisabled("Double-click to import just that file.");
                for (const std::string& rel : s.ModelFiles) {
                    const std::string full = (fs::path(s.Path) / rel).string();
                    ImGui::Selectable((ICON_FA_CUBE "  " + rel).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_EditorCameraPtr)
                        HandleDroppedFiles(world, assets, *m_EditorCameraPtr, true, {full});
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}
