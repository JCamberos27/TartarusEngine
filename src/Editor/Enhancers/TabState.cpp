#include "TabState.h"

#include "AtomicFile.h"
#include "EnhancerUserState.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "UserPaths.h"

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace Enhancers {

// --- TabStrip ---------------------------------------------------------------------------------
int TabStrip::Open(const EditorRef& r, bool activate, bool allowDuplicate) {
    int i = allowDuplicate ? -1 : Find(r);
    if (i >= 0) {
        Tabs[(std::size_t)i].Label = r.Label.empty() ? Tabs[(std::size_t)i].Label : r.Label;
    } else {
        if (Tabs.size() >= kMaxTabs) Close(Active == 0 ? 1 : 0); // oldest-ish, never the shown one
        i = (Active >= 0 && Active < (int)Tabs.size()) ? Active + 1 : (int)Tabs.size();
        Tabs.insert(Tabs.begin() + i, r);
        if (Active >= i) ++Active;
    }
    if (activate) Active = i;
    return i;
}

bool TabStrip::Close(int i) {
    if (i < 0 || i >= (int)Tabs.size()) return false;
    Closed.erase(std::remove(Closed.begin(), Closed.end(), Tabs[(std::size_t)i]), Closed.end());
    Closed.push_back(Tabs[(std::size_t)i]);
    if (Closed.size() > kMaxClosed) Closed.erase(Closed.begin());
    Tabs.erase(Tabs.begin() + i);
    if (Active == i) Active = i < (int)Tabs.size() ? i : (int)Tabs.size() - 1;
    else if (Active > i) --Active;
    return true;
}

void TabStrip::CloseOthers(int keep) {
    for (int i = (int)Tabs.size() - 1; i >= 0; --i) {
        if (i == keep) continue;
        Close(i);
        if (keep > i) --keep;
    }
    Active = keep >= 0 && keep < (int)Tabs.size() ? keep : -1;
}

int TabStrip::Reopen() {
    while (!Closed.empty()) {
        EditorRef r = std::move(Closed.back());
        Closed.pop_back();
        if (Find(r) < 0) return Open(r, true);
    }
    return -1;
}

void TabStrip::Move(int from, int to) {
    if (from < 0 || from >= (int)Tabs.size()) return;
    to = std::clamp(to, 0, (int)Tabs.size() - 1);
    if (from == to) return;
    const bool wasActive = Active == from;
    MoveRef(Tabs, from, to);
    if (wasActive) Active = to;
    else if (Active > from && Active <= to) --Active;
    else if (Active < from && Active >= to) ++Active;
}

int TabStrip::Step(int dir, bool allowNone) const {
    const int lo = allowNone ? -1 : 0, hi = (int)Tabs.size() - 1;
    if (hi < lo) return Active;
    const int n = hi - lo + 1;
    const int cur = std::clamp(Active, lo, hi);
    return lo + (((cur - lo + (dir > 0 ? 1 : -1)) % n) + n) % n;
}

json TabStrip::ToJson() const {
    return json{{"tabs", RefsToJson(Tabs)}, {"active", Active}, {"closed", RefsToJson(Closed)}};
}

void TabStrip::FromJson(const json& j) {
    *this = TabStrip{};
    if (!j.is_object()) return;
    if (auto it = j.find("tabs"); it != j.end()) Tabs = RefsFromJson(*it);
    if (auto it = j.find("closed"); it != j.end()) Closed = RefsFromJson(*it);
    if (Tabs.size() > kMaxTabs) Tabs.resize(kMaxTabs);
    if (Closed.size() > kMaxClosed) Closed.erase(Closed.begin(), Closed.end() - (std::ptrdiff_t)kMaxClosed);
    if (auto it = j.find("active"); it != j.end() && it->is_number_integer()) Active = it->get<int>();
    if (Active < -1 || Active >= (int)Tabs.size()) Active = Tabs.empty() ? -1 : 0;
}

// --- TabState ---------------------------------------------------------------------------------
TabState& TabState::Get() {
    static TabState s;
    return s;
}

std::string TabState::Path() {
    return UserPaths::Resolve("enhancer_tabs_" + EnhancerUserState::ProjectKey(ProjectPaths::Root()) + ".json");
}

void TabState::Reset() {
    Inspector = TabStrip{};
    Assets = TabStrip{};
    Starred.clear();
    m_Dirty = false;
}

json TabState::ToJson() const {
    return json{{"version", 1}, {"inspector", Inspector.ToJson()}, {"assets", Assets.ToJson()}, {"starred", RefsToJson(Starred)}};
}

void TabState::FromJson(const json& root) {
    Reset();
    if (!root.is_object()) return;
    if (auto it = root.find("inspector"); it != root.end()) Inspector.FromJson(*it);
    if (auto it = root.find("assets"); it != root.end()) Assets.FromJson(*it);
    if (auto it = root.find("starred"); it != root.end()) Starred = RefsFromJson(*it);
}

void TabState::Load() {
    std::ifstream in(Path());
    if (!in.is_open()) { Reset(); return; }
    try {
        json root;
        in >> root;
        FromJson(root);
    } catch (const std::exception& e) {
        Log::Warn(std::string("vTabs: failed to parse '") + Path() + "': " + e.what() + " - starting empty.");
        Reset();
    }
}

void TabState::Flush() {
    if (!m_Dirty) return;
    m_Dirty = false;
    // The global undo journals only project files and the whitelisted preference files, so this
    // per-user file stays out of the History panel: switching tabs is navigation, not an edit.
    if (!AtomicFile::WriteJson(Path(), ToJson()))
        Log::Warn(std::string("vTabs: could not write '") + Path() + "'.");
}

} // namespace Enhancers
