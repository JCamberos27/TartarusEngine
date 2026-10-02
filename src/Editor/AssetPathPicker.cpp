#include "AssetPathPicker.h"

#include "EditorLayerInternal.h"
#include "EditorTheme.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "FileDialog.h"
#include "ProjectPaths.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace AssetExts {
const char* const Images[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr", nullptr};
const char* const Hdri[] = {".hdr", ".exr", nullptr};
const char* const Models[] = {".fbx", ".gltf", ".glb", ".obj", ".dae", nullptr};
const char* const Sounds[] = {".wav", ".mp3", ".ogg", ".flac", nullptr};
const char* const Materials[] = {".mat", nullptr};
const char* const Animations[] = {".fbx", ".gltf", ".glb", ".dae", nullptr};
}

namespace {

struct ProjectScan {
    std::vector<std::string> Files;   // project-relative, forward slashes, sorted
    std::vector<std::string> Folders; // likewise
};

// One walk of the project folder, shared by every picker, at most every few seconds.
const ProjectScan& Scan() {
    static ProjectScan scan;
    static std::chrono::steady_clock::time_point at{};
    static bool once = false;
    const auto now = std::chrono::steady_clock::now();
    if (once && now - at < std::chrono::seconds(4)) return scan;
    once = true;
    at = now;
    scan.Files.clear();
    scan.Folders.clear();
    std::error_code ec;
    const fs::path root(ProjectPaths::Root());
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory()) {
            const std::string name = p.filename().string();
            if ((it.depth() == 0 && name == "Library") || (!name.empty() && name[0] == '.')) {
                it.disable_recursion_pending();
                continue;
            }
            scan.Folders.push_back(p.lexically_relative(root).generic_u8string());
            continue;
        }
        if (!it->is_regular_file() || p.extension() == ".meta") continue;
        scan.Files.push_back(p.lexically_relative(root).generic_u8string());
    }
    std::sort(scan.Files.begin(), scan.Files.end());
    std::sort(scan.Folders.begin(), scan.Folders.end());
    return scan;
}

bool HasExtension(const std::string& path, const char* const* exts) {
    if (!exts) return true;
    std::string ext = fs::u8path(path).extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    for (const char* const* e = exts; *e; ++e)
        if (ext == *e) return true;
    return false;
}

// Every space-separated word of `filter` appears in `text` (case-insensitive).
bool Matches(const char* filter, const std::string& text) {
    if (!filter || !*filter) return true;
    std::string hay = text;
    for (char& c : hay) c = (char)std::tolower((unsigned char)c);
    std::string word;
    for (const char* p = filter;; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!word.empty() && hay.find(word) == std::string::npos) return false;
            word.clear();
            if (*p == '\0') break;
        } else {
            word += (char)std::tolower((unsigned char)*p);
        }
    }
    return true;
}

std::string StoredForm(const std::string& absOrRel, bool keepAbsolute) {
    if (keepAbsolute) return absOrRel;
    const fs::path p = fs::u8path(absOrRel);
    if (!p.is_absolute()) return absOrRel;
    const std::string rel = ProjectPaths::Relativize(absOrRel);
    return fs::u8path(rel).is_absolute() ? absOrRel : rel;
}

} // namespace

bool AssetPathPicker(const char* id, std::string& path, const AssetPathPickerOptions& opt) {
    bool changed = false;
    ImGui::PushID(id);
    const float total = ImGui::CalcItemWidth();
    const float browseW = ImGui::GetFrameHeight();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;

    // The dropdown, showing the file's name (its folder in the tooltip), red when it's missing.
    const fs::path resolved = path.empty() ? fs::path() : fs::u8path(ProjectPaths::Resolve(path));
    std::error_code ec;
    const bool missing = !path.empty() && !fs::exists(resolved, ec);
    std::string preview = path.empty() ? std::string(opt.NoneLabel ? opt.NoneLabel : "(none)")
                                       : fs::u8path(path).filename().u8string();
    if (opt.Folder && !path.empty() && preview.empty()) preview = path;
    if (missing) preview = std::string(ICON_FA_TRIANGLE_EXCLAMATION "  ") + preview;
    ImGui::SetNextItemWidth(std::max(1.0f, total - browseW - gap));
    if (missing) ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Danger);
    else if (path.empty()) ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Dim);
    else ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Text);
    const bool open = ImGui::BeginCombo("##pick", preview.c_str(), ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleColor();
    if (!open && ImGui::IsItemHovered() && !path.empty())
        EditorUI::SetTooltip(missing ? "Missing: %s" : "%s", path.c_str());

    if (opt.DragPayload && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(opt.DragPayload)) {
            const std::string dropped = StoredForm((const char*)p->Data, opt.KeepAbsolute);
            if (opt.Folder || HasExtension(dropped, opt.Extensions)) {
                path = dropped;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    bool browse = false;
    if (open && opt.BrowseOnly) {
        if (opt.AllowNone && ImGui::Selectable(opt.NoneLabel ? opt.NoneLabel : "(none)", path.empty())) {
            path.clear();
            changed = true;
        }
        if (ImGui::Selectable(opt.Folder ? ICON_FA_FOLDER_OPEN "  Choose a folder..." : ICON_FA_FILE_ARROW_UP "  Choose a file..."))
            browse = true;
        ImGui::EndCombo();
    } else if (open) {
        static char s_filter[128] = {};
        if (ImGui::IsWindowAppearing()) {
            s_filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        EditorUIPrimitives::SearchField("##filter", s_filter, sizeof(s_filter), opt.Folder ? "Search folders" : "Search files");
        ImGui::Separator();
        if (opt.AllowNone && ImGui::Selectable(opt.NoneLabel ? opt.NoneLabel : "(none)", path.empty())) {
            path.clear();
            changed = true;
        }
        const ProjectScan& scan = Scan();
        const std::vector<std::string>& list = opt.Folder ? scan.Folders : scan.Files;
        int shown = 0, matched = 0;
        const int kMaxShown = 250;
        for (const std::string& entry : list) {
            if (!opt.Folder && !HasExtension(entry, opt.Extensions)) continue;
            if (!Matches(s_filter, entry)) continue;
            ++matched;
            if (shown >= kMaxShown) continue;
            ++shown;
            const fs::path ep = fs::u8path(entry);
            const std::string name = opt.Folder ? entry : ep.filename().u8string();
            const std::string folder = opt.Folder ? std::string() : ep.parent_path().generic_u8string();
            ImGui::PushID(entry.c_str());
            if (ImGui::Selectable("##row", entry == path, 0, ImVec2(0.0f, ImGui::GetTextLineHeight()))) {
                path = entry;
                changed = true;
            }
            // Name in the text colour, its folder after it dimmed.
            const ImVec2 mn = ImGui::GetItemRectMin();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(mn, EditorTheme::U32(EditorTheme::Text), name.c_str());
            if (!folder.empty()) {
                const float nx = mn.x + ImGui::CalcTextSize(name.c_str()).x + EditorTheme::Px(10.0f);
                dl->AddText(ImVec2(nx, mn.y), EditorTheme::U32(EditorTheme::Dim), folder.c_str());
            }
            ImGui::PopID();
        }
        if (matched == 0) EditorInternal::HintText(opt.Folder ? "No folders match." : "No project files match.");
        else if (matched > shown) {
            char more[64];
            std::snprintf(more, sizeof(more), "%d more - type to narrow the list", matched - shown);
            EditorInternal::HintText(more);
        }
        ImGui::EndCombo();
    }

    // Browse... for a file (or folder) anywhere on disk.
    ImGui::SameLine(0.0f, gap);
    if (EditorUIPrimitives::ActionButton(opt.Folder ? ICON_FA_FOLDER_OPEN : ICON_FA_FILE_ARROW_UP,
                                         opt.Folder ? "Choose a folder..." : "Choose a file...",
                                         &EditorInternal::ForwardHostTooltip, false, ImVec2(browseW, browseW)))
        browse = true;
    if (browse) {
        const std::string picked = opt.Folder ? FileDialog::PickFolder(opt.Owner)
                                              : FileDialog::OpenFile(opt.DialogFilter ? opt.DialogFilter : "All Files\0*.*\0", opt.Owner);
        if (!picked.empty()) {
            path = StoredForm(picked, opt.KeepAbsolute);
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}
