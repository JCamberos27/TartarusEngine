#include "AssetPathPicker.h"

#include "EditorLayerInternal.h"
#include "EditorTheme.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "FileDialog.h"
#include "Core/Paths.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace AssetExts {
const char* const Images[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr", nullptr};
const char* const Hdri[] = {".hdr", ".exr", nullptr};
const char* const Models[] = {".fbx", ".gltf", ".glb", ".obj", ".dae", nullptr};
const char* const Materials[] = {".mat", nullptr};
const char* const Animations[] = {".fbx", ".gltf", ".glb", ".dae", nullptr};
}

namespace {

struct ScanEntry {
    std::string Path;  // project-relative, forward slashes
    std::string Lower; // Path lower-cased, for the filter
    std::string Ext;   // lower-cased extension with its dot
};
struct ProjectScan {
    std::vector<ScanEntry> Files;   // sorted by Path
    std::vector<ScanEntry> Folders; // likewise
};

ScanEntry MakeEntry(std::string path, bool folder) {
    ScanEntry e{std::move(path), {}, {}};
    e.Lower = e.Path;
    for (char& c : e.Lower) c = (char)std::tolower((unsigned char)c);
    if (!folder) {
        const size_t dot = e.Lower.find_last_of('.'), slash = e.Lower.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) e.Ext = e.Lower.substr(dot);
    }
    return e;
}

// One walk of the project folder, shared by every picker: taken when a picker's list opens and the
// last one is more than a few seconds old, never while a list is just staying open (a walk of a big
// project is a visible hitch).
const ProjectScan& Scan(bool opening) {
    static ProjectScan scan;
    static std::chrono::steady_clock::time_point at{};
    static bool once = false;
    const auto now = std::chrono::steady_clock::now();
    if (once && (!opening || now - at < std::chrono::seconds(4))) return scan;
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
            scan.Folders.push_back(MakeEntry(p.lexically_relative(root).generic_u8string(), true));
            continue;
        }
        if (!it->is_regular_file() || p.extension() == ".meta") continue;
        scan.Files.push_back(MakeEntry(p.lexically_relative(root).generic_u8string(), false));
    }
    const auto byPath = [](const ScanEntry& a, const ScanEntry& b) { return a.Path < b.Path; };
    std::sort(scan.Files.begin(), scan.Files.end(), byPath);
    std::sort(scan.Folders.begin(), scan.Folders.end(), byPath);
    return scan;
}

bool HasLowerExtension(const std::string& ext, const char* const* exts) {
    if (!exts) return true;
    for (const char* const* e = exts; *e; ++e)
        if (ext == *e) return true;
    return false;
}

bool HasExtension(const std::string& path, const char* const* exts) {
    if (!exts) return true;
    std::string ext = fs::u8path(path).extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return HasLowerExtension(ext, exts);
}

// Whether the picker's current file is missing, re-asked at most once a second per path (every
// picker on screen asked the disk every frame).
bool PathMissing(const std::string& path) {
    struct Seen { bool Missing; std::chrono::steady_clock::time_point At; };
    static std::unordered_map<std::string, Seen> s_Seen;
    const auto now = std::chrono::steady_clock::now();
    auto it = s_Seen.find(path);
    if (it != s_Seen.end() && now - it->second.At < std::chrono::seconds(1)) return it->second.Missing;
    if (s_Seen.size() > 512) s_Seen.clear();
    std::error_code ec;
    const bool missing = !fs::exists(fs::u8path(ProjectPaths::Resolve(path)), ec);
    s_Seen[path] = {missing, now};
    return missing;
}

// Every space-separated word of `filter` appears in `lowerText` (already lower-cased).
bool Matches(const char* filter, const std::string& lowerText) {
    if (!filter || !*filter) return true;
    const std::string& hay = lowerText;
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
    const bool missing = !path.empty() && PathMissing(path);
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
        const bool appearing = ImGui::IsWindowAppearing();
        if (appearing) {
            s_filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        EditorUIPrimitives::SearchField("##filter", s_filter, sizeof(s_filter), opt.Folder ? "Search folders" : "Search files");
        ImGui::Separator();
        if (opt.AllowNone && ImGui::Selectable(opt.NoneLabel ? opt.NoneLabel : "(none)", path.empty())) {
            path.clear();
            changed = true;
        }
        const ProjectScan& scan = Scan(appearing);
        const std::vector<ScanEntry>& list = opt.Folder ? scan.Folders : scan.Files;
        int shown = 0, matched = 0;
        const int kMaxShown = 250;
        for (const ScanEntry& scanned : list) {
            if (!opt.Folder && !HasLowerExtension(scanned.Ext, opt.Extensions)) continue;
            if (!Matches(s_filter, scanned.Lower)) continue;
            const std::string& entry = scanned.Path;
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
