#pragma once
#include <json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Editor Enhancers - the shared, pure-logic layer under the vHierarchy / vFolders / vInspector /
// vTabs / vFavorites / vRuler style features (docs/EDITOR_ENHANCERS.md).
//
// Deliberately free of ImGui and EnTT: everything here is plain data + algorithms, so the unit
// tests link it directly and the per-feature UI code stays a thin layer over tested behaviour.
// The editor-side resolution of a ref to a live entity / asset lives with the features that need
// it (EditorLayer), not here.
namespace Enhancers {

// --- EditorRef ------------------------------------------------------------------------------
// A persistent pointer at something the user can bookmark, tab or favorite. Never holds an
// entt::entity: entt ids are recycled across undo, Play/Stop and scene reloads, so an entity is
// named the way the rest of the editor names it persistently - the scene's GUID plus the
// entity's OrderComponent value (see Components.h OrderComponent).
enum class RefKind : std::uint8_t { Entity = 0, Asset = 1, Folder = 2, Scene = 3 };

struct EditorRef {
    RefKind     Kind  = RefKind::Asset;
    std::string Scene;      // Entity: owning scene GUID (16 hex). Scene: the scene's GUID.
    int         Order = -1; // Entity only: OrderComponent value
    std::string Path;       // Asset: asset key. Folder: virtual folder path. Scene: file path (fallback when the GUID is unknown).
    std::string Label;      // cached display name, refreshed whenever the ref resolves; shown when it doesn't
    // Entity only, optional: a component on it (ComponentRegistry name) - a vTabs component tab.
    // Part of the identity: the entity and one of its components are different targets.
    std::string Sub;

    static EditorRef MakeEntity(std::string sceneGuid, int order, std::string label = {});
    static EditorRef MakeAsset(std::string key, std::string label = {});
    static EditorRef MakeFolder(std::string path, std::string label = {});
    static EditorRef MakeScene(std::string guid, std::string path, std::string label = {});
    static EditorRef MakeComponent(std::string sceneGuid, int order, std::string component, std::string label = {});

    // Identity, not display: the cached Label never participates.
    bool SameTarget(const EditorRef& o) const;
    bool operator==(const EditorRef& o) const { return SameTarget(o); }
    bool operator!=(const EditorRef& o) const { return !SameTarget(o); }
};

nlohmann::json RefToJson(const EditorRef& r);
// False (and `out` untouched) for a malformed or unknown-kind object - one bad row in a
// hand-edited file must not take the whole list down with it.
bool RefFromJson(const nlohmann::json& j, EditorRef& out);

nlohmann::json RefsToJson(const std::vector<EditorRef>& refs);
std::vector<EditorRef> RefsFromJson(const nlohmann::json& j);

// --- Bookmark lists ---------------------------------------------------------------------------
// Ordered, de-duplicated lists (nav-bar chips, favorites pages, tab strips). Adding an existing
// target refreshes its label in place instead of duplicating it. `cap` drops the oldest (front).
bool AddUnique(std::vector<EditorRef>& list, const EditorRef& r, std::size_t cap = 64);
bool RemoveRef(std::vector<EditorRef>& list, const EditorRef& r);
int  FindRef(const std::vector<EditorRef>& list, const EditorRef& r); // -1 when absent
// Moves list[from] so it ends up at index `to` (clamped). No-op on a bad `from`.
void MoveRef(std::vector<EditorRef>& list, int from, int to);

// --- Back/Forward history -------------------------------------------------------------------
// Browser-style: Push after Back drops the forward entries; pushing the current entry again is
// a no-op; the oldest entry falls off at `Cap`.
template <class T>
class NavHistory {
public:
    explicit NavHistory(std::size_t cap = 64) : m_Cap(cap < 1 ? 1 : cap) {}

    void Push(const T& v) {
        if (m_Cursor >= 0 && m_Entries[(std::size_t)m_Cursor] == v) return;
        m_Entries.resize((std::size_t)(m_Cursor + 1)); // truncate forward
        m_Entries.push_back(v);
        if (m_Entries.size() > m_Cap) m_Entries.erase(m_Entries.begin());
        m_Cursor = (int)m_Entries.size() - 1;
    }
    bool CanBack() const    { return m_Cursor > 0; }
    bool CanForward() const { return m_Cursor + 1 < (int)m_Entries.size(); }
    // Returns the entry moved to, or nullptr at either end.
    const T* Back()    { if (!CanBack()) return nullptr;    return &m_Entries[(std::size_t)--m_Cursor]; }
    const T* Forward() { if (!CanForward()) return nullptr; return &m_Entries[(std::size_t)++m_Cursor]; }
    const T* Current() const { return m_Cursor >= 0 ? &m_Entries[(std::size_t)m_Cursor] : nullptr; }
    void Clear() { m_Entries.clear(); m_Cursor = -1; }
    std::size_t Size() const { return m_Entries.size(); }
    int Cursor() const { return m_Cursor; }
    const std::vector<T>& Entries() const { return m_Entries; }

private:
    std::vector<T> m_Entries;
    int            m_Cursor = -1;
    std::size_t    m_Cap;
};

// --- Selection history (vInspector Back/Forward) ---------------------------------------------
// One past selection, named by identity rather than by entt handle so it survives undo,
// Play/Stop and scene loads: the scene key plus each selected entity's OrderComponent value
// (primary first), or an asset key when only an asset was selected (Scene left empty: an asset
// selection belongs to no scene). An entry with neither is an empty selection in `Scene`.
struct SelectionHistoryEntry {
    std::string      Scene;
    std::vector<int> Orders;
    std::string      Asset;
    bool operator==(const SelectionHistoryEntry& o) const {
        return Orders == o.Orders && Asset == o.Asset && Scene == o.Scene;
    }
    bool operator!=(const SelectionHistoryEntry& o) const { return !(*this == o); }
};

// Whether Back/Forward may land on `e` while `currentScene` is open: an asset entry always can;
// an entity or empty entry only within its own scene. `resolves` (optional) further rejects
// entity entries none of whose orders exist any more.
using OrdersResolveFn = bool (*)(const std::vector<int>& orders, void* ctx);
bool SelectionEntryReachable(const SelectionHistoryEntry& e, const std::string& currentScene,
                             OrdersResolveFn resolves = nullptr, void* ctx = nullptr);
// Index of the nearest reachable entry from `pos` in direction `dir` (-1 / +1), skipping
// unreachable ones and ones equal to entries[pos] (a no-op step); -1 when there is none.
int StepSelectionHistory(const std::vector<SelectionHistoryEntry>& entries, int pos, int dir,
                         const std::string& currentScene, OrdersResolveFn resolves = nullptr,
                         void* ctx = nullptr);

// --- Matching ---------------------------------------------------------------------------------
// Case-insensitive glob: '*' any run (including empty, and including '/'), '?' one character.
// Used by vFolders style rules ("*Materials*", "Art/*/Textures").
bool GlobMatch(const char* pattern, const char* text);

// Fuzzy subsequence score for the vTabs "+" search and other quick pickers. Returns -1 when the
// query's characters don't all appear in order. Higher is better: bonuses for consecutive runs,
// for matching at a word start (after ' ', '_', '-', '/', '.', or a lower->Upper transition),
// for matching the very first character, and a small penalty for each skipped character, so
// "pc" ranks "PlayerController" above "SpaceCrate". An empty query scores 0 (matches all).
int FuzzyScore(const char* query, const char* text);

// --- Folder-path keyed maps -------------------------------------------------------------------
// Re-keys every entry for `oldPath` or anything under it ("oldPath/...") to live under `newPath`
// - a folder rename/move. An empty `newPath` drops those entries (folder deleted). Matches whole
// path segments only: renaming "Art" never touches "Artwork". Returns the number re-keyed.
template <class V>
int RemapFolderKeys(std::map<std::string, V>& m, const std::string& oldPath, const std::string& newPath) {
    if (oldPath.empty() || oldPath == newPath) return 0;
    std::vector<std::pair<std::string, V>> moved;
    int n = 0;
    for (auto it = m.begin(); it != m.end();) {
        const std::string& k = it->first;
        const bool exact = k == oldPath;
        const bool under = !exact && k.size() > oldPath.size() && k.compare(0, oldPath.size(), oldPath) == 0 &&
                           k[oldPath.size()] == '/';
        if (!exact && !under) { ++it; continue; }
        if (!newPath.empty()) moved.emplace_back(newPath + k.substr(oldPath.size()), std::move(it->second));
        it = m.erase(it);
        ++n;
    }
    for (auto& kv : moved) m[kv.first] = std::move(kv.second);
    return n;
}

// Same rule for a plain list of folder paths (bookmark chips, expanded-state sets).
int RemapFolderPath(std::string& path, const std::string& oldPath, const std::string& newPath);

// --- Tree lines -------------------------------------------------------------------------------
// vHierarchy/vFolders tree guides. A row's mask has bit k set when the vertical guide at nesting
// level k continues past it (bit Depth-1 = the row itself has a later sibling; lower bits = an
// ancestor at that level does). A child of a row at `parentDepth` inherits the parent's bits for
// the levels above it and sets bit `parentDepth` unless it is its parent's last child.
inline std::uint32_t TreeLineChildMask(std::uint32_t parentMask, int parentDepth, bool isLastChild) {
    if (parentDepth < 0) parentDepth = 0;
    const std::uint32_t inherited = parentDepth > 0 ? (parentMask & ((1u << (parentDepth > 31 ? 31 : parentDepth)) - 1u)) : 0u;
    return (!isLastChild && parentDepth < 32) ? (inherited | (1u << parentDepth)) : inherited;
}

// --- Units ------------------------------------------------------------------------------------
// Distance readout shared by the Measure tool and vRuler. Scene units are metres.
//   metric:   adapts the unit - "4.2 mm", "37.5 cm", "1.84 m", "2.31 km"
//   imperial: feet + inches - "5' 10.9\"", "0' 3.2\"", or miles past 5280 ft ("1.42 mi")
// Writes into `buf` (always NUL-terminated) and returns it.
const char* FormatLength(float meters, bool imperial, char* buf, std::size_t n);

// --- Font Awesome icon names ------------------------------------------------------------------
// Styles store icons by FA name ("cube", "folder-open"), not codepoint - see gen_fa_icon_table.py.
struct FAIcon { const char* Name; const char* Glyph; };
const FAIcon* FAIconTable(std::size_t* count);   // sorted by Name
const char* FAIconGlyph(const char* name);       // nullptr when unknown (or name null/empty)

} // namespace Enhancers
