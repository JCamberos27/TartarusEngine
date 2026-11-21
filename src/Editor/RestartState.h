#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>

class World;

// Rebuild & Restart (File menu, Ctrl+Alt+R): what the editor hands to the freshly built copy of
// itself so the restart lands where the user left off. Written once just before the old process
// exits, read and deleted by the new one (--restore-state <file>). The camera, dock layout and panel
// prefs need nothing here: the normal clean shutdown already saves them.
namespace RestartState {

// A selected entity, found again after the reload by its OrderComponent value (kept through save and
// the recovery snapshot) and checked against its name, so a mismatch drops it instead of selecting
// something else.
struct EntityRef {
    int Order = 0;
    std::string Name;
};

struct State {
    std::string ScenePath;        // the scene that was open ("" = an unsaved new scene)
    bool RestoreRecovery = false; // the scene had unsaved edits: load its recovery snapshot, no prompt
    bool WasPlaying = false;      // re-enter Play after the load (the play session itself starts fresh)
    std::vector<EntityRef> Selection;
    std::int64_t SavedAtUnix = 0; // when it was written; old files are ignored
};

// Files older than this are leftovers (a relaunch that never happened) and are ignored.
constexpr std::int64_t kMaxAgeSeconds = 300;

std::int64_t NowUnix();

// Writes `state` to `file` (stamping SavedAtUnix when it is 0). False on any write error.
bool Save(const std::filesystem::path& file, const State& state);
// Reads `file`. nullopt (with `why` filled) when missing, unreadable, malformed or older than
// kMaxAgeSeconds as of `nowUnix`.
std::optional<State> Load(const std::filesystem::path& file, std::int64_t nowUnix, std::string* why = nullptr);

// The references for `entities` (entities without an OrderComponent are skipped).
std::vector<EntityRef> RefsFor(const World& world, const std::vector<entt::entity>& entities);
// The live entities those references point at, in the same order; unmatched ones are dropped.
std::vector<entt::entity> Resolve(const World& world, const std::vector<EntityRef>& refs);

} // namespace RestartState
