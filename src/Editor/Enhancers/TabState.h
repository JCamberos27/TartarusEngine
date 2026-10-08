#pragma once
#include "EnhancerCore.h"

#include <json.hpp>

#include <string>
#include <vector>

// Editor Enhancers / vTabs - the tab strips in the Inspector and the Asset Browser.
//
// TabStrip is pure list logic (open / close / reopen / move / step) and is unit tested. TabState
// is the per-user, per-project store, deliberately a file of its own (enhancer_tabs_<hash>.json)
// rather than part of EnhancerUserState: that file is journaled by the global undo, and switching
// tabs - or the Asset Browser tab following folder navigation - is navigation, not an edit that
// should land in the History panel.
namespace Enhancers {

struct TabStrip {
    std::vector<EditorRef> Tabs;
    // Index of the shown tab; -1 = none (the Inspector's live "Selection" tab).
    int Active = -1;
    // Recently closed tabs, most recent last (Ctrl+Shift+T reopens).
    std::vector<EditorRef> Closed;

    static constexpr std::size_t kMaxTabs = 32;
    static constexpr std::size_t kMaxClosed = 16;

    // Opens `r` after the active tab (or at the end); an already-open target is reused unless
    // `allowDuplicate` (the Asset Browser's "new tab here"). Returns its index; it becomes
    // active when `activate`.
    int Open(const EditorRef& r, bool activate = true, bool allowDuplicate = false);
    // Closes tab `i`, remembering it for Reopen. The active tab moves to the right neighbour,
    // else the left one, else -1. False for a bad index.
    bool Close(int i);
    // Closes every tab but `keep` (all of them for -1).
    void CloseOthers(int keep);
    // Reopens the most recently closed tab that isn't open already; -1 when there is none.
    int Reopen();
    // Moves tab `from` to index `to` (clamped), keeping the same tab active.
    void Move(int from, int to);
    // The tab `dir` steps from Active, wrapping. With `allowNone`, -1 (Selection) is part of the
    // cycle. Returns the new index without changing Active.
    int Step(int dir, bool allowNone) const;
    int Find(const EditorRef& r) const { return FindRef(Tabs, r); }

    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& j);
};

class TabState {
public:
    static TabState& Get();

    TabStrip Inspector; // entity / component / asset targets; Active -1 = follow the selection
    TabStrip Assets;    // folders; the active tab follows the Asset Browser's navigation
    std::vector<EditorRef> Starred; // offered at the top of each strip's "+" menu

    void Load();
    void Reset();
    void MarkDirty() { m_Dirty = true; }
    void Flush(); // at most one write per call; call once per frame

    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& root);
    // %LOCALAPPDATA%\TartarusEngine\enhancer_tabs_<project hash>.json
    static std::string Path();

private:
    TabState() = default;
    bool m_Dirty = false;
};

} // namespace Enhancers
