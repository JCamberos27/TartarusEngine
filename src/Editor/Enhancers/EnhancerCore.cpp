#include "EnhancerCore.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace Enhancers {

// --- EditorRef ------------------------------------------------------------------------------
EditorRef EditorRef::MakeEntity(std::string sceneGuid, int order, std::string label) {
    EditorRef r; r.Kind = RefKind::Entity; r.Scene = std::move(sceneGuid); r.Order = order; r.Label = std::move(label);
    return r;
}
EditorRef EditorRef::MakeAsset(std::string key, std::string label) {
    EditorRef r; r.Kind = RefKind::Asset; r.Path = std::move(key); r.Label = std::move(label);
    return r;
}
EditorRef EditorRef::MakeFolder(std::string path, std::string label) {
    EditorRef r; r.Kind = RefKind::Folder; r.Path = std::move(path); r.Label = std::move(label);
    return r;
}
EditorRef EditorRef::MakeScene(std::string guid, std::string path, std::string label) {
    EditorRef r; r.Kind = RefKind::Scene; r.Scene = std::move(guid); r.Path = std::move(path); r.Label = std::move(label);
    return r;
}

bool EditorRef::SameTarget(const EditorRef& o) const {
    if (Kind != o.Kind) return false;
    switch (Kind) {
        case RefKind::Entity: return Order == o.Order && Scene == o.Scene;
        // A scene is its GUID when both sides know it; a scene saved before it had a .meta (or
        // a ref written while the asset scan was still running) falls back to the path.
        case RefKind::Scene:  return (!Scene.empty() && !o.Scene.empty()) ? Scene == o.Scene : Path == o.Path;
        case RefKind::Asset:
        case RefKind::Folder: return Path == o.Path;
    }
    return false;
}

namespace {
const char* KindName(RefKind k) {
    switch (k) {
        case RefKind::Entity: return "entity";
        case RefKind::Asset:  return "asset";
        case RefKind::Folder: return "folder";
        case RefKind::Scene:  return "scene";
    }
    return "asset";
}
bool KindFromName(const std::string& s, RefKind& out) {
    if (s == "entity") { out = RefKind::Entity; return true; }
    if (s == "asset")  { out = RefKind::Asset;  return true; }
    if (s == "folder") { out = RefKind::Folder; return true; }
    if (s == "scene")  { out = RefKind::Scene;  return true; }
    return false;
}
std::string StrOr(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
} // namespace

nlohmann::json RefToJson(const EditorRef& r) {
    nlohmann::json j;
    j["kind"] = KindName(r.Kind);
    if (!r.Scene.empty()) j["scene"] = r.Scene;
    if (r.Kind == RefKind::Entity) j["order"] = r.Order;
    if (!r.Path.empty()) j["path"] = r.Path;
    if (!r.Label.empty()) j["label"] = r.Label;
    return j;
}

bool RefFromJson(const nlohmann::json& j, EditorRef& out) {
    if (!j.is_object()) return false;
    EditorRef r;
    if (!KindFromName(StrOr(j, "kind"), r.Kind)) return false;
    r.Scene = StrOr(j, "scene");
    r.Path  = StrOr(j, "path");
    r.Label = StrOr(j, "label");
    if (r.Kind == RefKind::Entity) {
        auto it = j.find("order");
        if (it == j.end() || !it->is_number_integer()) return false;
        r.Order = it->get<int>();
    }
    // Every kind needs *something* to resolve by.
    const bool usable = r.Kind == RefKind::Entity ? true
                      : r.Kind == RefKind::Scene  ? (!r.Scene.empty() || !r.Path.empty())
                      : r.Kind == RefKind::Folder ? true // "" is the project root folder
                      : !r.Path.empty();
    if (!usable) return false;
    out = std::move(r);
    return true;
}

nlohmann::json RefsToJson(const std::vector<EditorRef>& refs) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& r : refs) a.push_back(RefToJson(r));
    return a;
}

std::vector<EditorRef> RefsFromJson(const nlohmann::json& j) {
    std::vector<EditorRef> out;
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (const auto& e : j) {
        EditorRef r;
        if (RefFromJson(e, r)) AddUnique(out, r, (std::size_t)-1);
    }
    return out;
}

// --- Bookmark lists ---------------------------------------------------------------------------
int FindRef(const std::vector<EditorRef>& list, const EditorRef& r) {
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].SameTarget(r)) return (int)i;
    return -1;
}

bool AddUnique(std::vector<EditorRef>& list, const EditorRef& r, std::size_t cap) {
    const int at = FindRef(list, r);
    if (at >= 0) {
        if (!r.Label.empty()) list[(std::size_t)at].Label = r.Label;
        return false;
    }
    list.push_back(r);
    while (cap > 0 && list.size() > cap) list.erase(list.begin());
    return true;
}

bool RemoveRef(std::vector<EditorRef>& list, const EditorRef& r) {
    const int at = FindRef(list, r);
    if (at < 0) return false;
    list.erase(list.begin() + at);
    return true;
}

void MoveRef(std::vector<EditorRef>& list, int from, int to) {
    if (from < 0 || from >= (int)list.size()) return;
    to = std::clamp(to, 0, (int)list.size() - 1);
    if (from == to) return;
    EditorRef v = std::move(list[(std::size_t)from]);
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, std::move(v));
}

// --- Selection history ------------------------------------------------------------------------
bool SelectionEntryReachable(const SelectionHistoryEntry& e, const std::string& currentScene,
                             OrdersResolveFn resolves, void* ctx) {
    if (e.Orders.empty() && !e.Asset.empty()) return true;
    if (e.Scene != currentScene) return false;
    return e.Orders.empty() || !resolves || resolves(e.Orders, ctx);
}

int StepSelectionHistory(const std::vector<SelectionHistoryEntry>& entries, int pos, int dir,
                         const std::string& currentScene, OrdersResolveFn resolves, void* ctx) {
    if (dir == 0 || entries.empty()) return -1;
    const int n = (int)entries.size();
    const SelectionHistoryEntry* from = (pos >= 0 && pos < n) ? &entries[(std::size_t)pos] : nullptr;
    for (int i = pos + (dir > 0 ? 1 : -1); i >= 0 && i < n; i += (dir > 0 ? 1 : -1)) {
        const SelectionHistoryEntry& e = entries[(std::size_t)i];
        if (from && e == *from) continue;
        if (SelectionEntryReachable(e, currentScene, resolves, ctx)) return i;
    }
    return -1;
}

// --- Matching ---------------------------------------------------------------------------------
namespace {
inline char Lower(char c) { return (char)std::tolower((unsigned char)c); }
} // namespace

bool GlobMatch(const char* pattern, const char* text) {
    if (!pattern || !text) return false;
    // Iterative wildcard match with single-star backtracking: linear in practice, and no
    // recursion depth to worry about on a pathological "*a*a*a*..." rule.
    const char* p = pattern; const char* t = text;
    const char* starP = nullptr; const char* starT = nullptr;
    while (*t) {
        if (*p == '*') { starP = ++p; starT = t; continue; }
        if (*p && (*p == '?' || Lower(*p) == Lower(*t))) { ++p; ++t; continue; }
        if (starP) { p = starP; t = ++starT; continue; }
        return false;
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

int FuzzyScore(const char* query, const char* text) {
    if (!query || !*query) return 0;
    if (!text) return -1;

    constexpr int kMatch       = 16;
    constexpr int kConsecutive = 24;
    constexpr int kWordStart   = 30;
    constexpr int kFirstChar   = 15;
    constexpr int kGap         = 1;  // per character skipped between two matches
    constexpr int kLeading     = 3;  // per character skipped before the first match...
    constexpr int kLeadingMax  = 9;  // ...capped, so a long common prefix can't sink everything
    constexpr int kMaxLen      = 256;
    constexpr int kNeg         = -1000000;

    const int q = (int)std::strlen(query);
    const int t = std::min((int)std::strlen(text), kMaxLen);
    if (q > t) return -1;

    auto bonusAt = [&](int j) {
        int b = kMatch;
        if (j == 0) return b + kFirstChar + kWordStart;
        const char prev = text[j - 1], cur = text[j];
        if (prev == ' ' || prev == '_' || prev == '-' || prev == '/' || prev == '.' || prev == '\\') b += kWordStart;
        else if (std::islower((unsigned char)prev) && std::isupper((unsigned char)cur)) b += kWordStart;
        else if (!std::isdigit((unsigned char)prev) && std::isdigit((unsigned char)cur)) b += kWordStart / 2;
        return b;
    };

    // Rolling DP: row[j] = best score with query[i] matched exactly at text[j]. The gap term is
    // kept O(q*t) with a running max of (prev[k] + k*kGap) over k <= j-2.
    int prev[kMaxLen], cur[kMaxLen];
    for (int j = 0; j < t; ++j)
        prev[j] = Lower(text[j]) == Lower(query[0]) ? bonusAt(j) - std::min(j * kLeading, kLeadingMax) : kNeg;

    for (int i = 1; i < q; ++i) {
        const char qc = Lower(query[i]);
        int runMax = kNeg; // max over k <= j-2 of prev[k] + k*kGap
        for (int j = 0; j < t; ++j) {
            if (j >= 2 && prev[j - 2] > kNeg) runMax = std::max(runMax, prev[j - 2] + (j - 2) * kGap);
            cur[j] = kNeg;
            if (j == 0 || Lower(text[j]) != qc) continue;
            int best = kNeg;
            if (prev[j - 1] > kNeg) best = prev[j - 1] + kConsecutive;
            if (runMax > kNeg) best = std::max(best, runMax - (j - 1) * kGap);
            if (best > kNeg) cur[j] = best + bonusAt(j);
        }
        std::memcpy(prev, cur, sizeof(int) * (std::size_t)t);
    }

    int best = kNeg;
    for (int j = 0; j < t; ++j) best = std::max(best, prev[j]);
    return best > kNeg ? std::max(best, 1) : -1;
}

int RemapFolderPath(std::string& path, const std::string& oldPath, const std::string& newPath) {
    if (oldPath.empty() || oldPath == newPath) return 0;
    const bool exact = path == oldPath;
    const bool under = !exact && path.size() > oldPath.size() && path.compare(0, oldPath.size(), oldPath) == 0 &&
                       path[oldPath.size()] == '/';
    if (!exact && !under) return 0;
    path = newPath.empty() ? std::string() : newPath + path.substr(oldPath.size());
    return 1;
}

// --- Units ------------------------------------------------------------------------------------
const char* FormatLength(float meters, bool imperial, char* buf, std::size_t n) {
    if (!buf || n == 0) return buf;
    if (!std::isfinite(meters)) { std::snprintf(buf, n, "--"); return buf; }
    const char* sign = meters < 0.0f ? "-" : "";
    const double m = std::fabs((double)meters);
    if (!imperial) {
        if (m < 0.01)        std::snprintf(buf, n, "%s%.1f mm", sign, m * 1000.0);
        else if (m < 1.0)    std::snprintf(buf, n, "%s%.1f cm", sign, m * 100.0);
        else if (m < 1000.0) std::snprintf(buf, n, "%s%.3f m", sign, m);
        else                 std::snprintf(buf, n, "%s%.2f km", sign, m / 1000.0);
        return buf;
    }
    const double inchesTotal = m / 0.0254;
    const double feetTotal = inchesTotal / 12.0;
    if (feetTotal >= 5280.0) { std::snprintf(buf, n, "%s%.2f mi", sign, feetTotal / 5280.0); return buf; }
    // Round to a tenth of an inch first, then split, so 11.96" reads 1' 0.0" not 0' 12.0".
    const double tenths = std::round(inchesTotal * 10.0);
    const long long feet = (long long)(tenths / 120.0);
    const double inches = (tenths - (double)feet * 120.0) / 10.0;
    std::snprintf(buf, n, "%s%lld' %.1f\"", sign, feet, inches);
    return buf;
}

// --- Font Awesome icon names ------------------------------------------------------------------
namespace {
const FAIcon kFAIcons[] = {
#define TARTARUS_FA_ICON(name, glyph) {name, glyph},
#include "FAIconTable.inc"
#undef TARTARUS_FA_ICON
};
} // namespace

const FAIcon* FAIconTable(std::size_t* count) {
    if (count) *count = sizeof(kFAIcons) / sizeof(kFAIcons[0]);
    return kFAIcons;
}

const char* FAIconGlyph(const char* name) {
    if (!name || !*name) return nullptr;
    const FAIcon* b = std::begin(kFAIcons);
    const FAIcon* e = std::end(kFAIcons);
    const FAIcon* it = std::lower_bound(b, e, name, [](const FAIcon& a, const char* n) { return std::strcmp(a.Name, n) < 0; });
    return (it != e && std::strcmp(it->Name, name) == 0) ? it->Glyph : nullptr;
}

} // namespace Enhancers
