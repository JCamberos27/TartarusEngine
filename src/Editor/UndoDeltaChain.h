#pragma once
#include <json.hpp>
#include <string>
#include <vector>
#include <utility>

// Delta-encoded snapshot stack for the editor's undo/redo history (#174 stage 2).
//
// A history stack used to hold one full serialized scene per entry - 100 entries of a multi-MB
// scene meant hundreds of MB of retained JSON text. Instead the stack is a chain anchored at
// its TOP entry:
//
//   - the top entry's state is held in full, once, in the caller's `baseJson`;
//   - every entry below the top stores a `Delta`: an RFC 6902 JSON Patch (nlohmann::json::diff,
//     dumped to text) that turns the state of the entry ABOVE it back into its own state.
//
//   stack:  [0] <-Delta- [1] <-Delta- [2] <-Delta- [top]        baseJson == state(top)
//
// Anchoring at the top - not the bottom - is what makes this cheap: the top is the only end
// either stack is ever pushed to or popped from, so Push re-encodes exactly one entry and Pop
// applies exactly one patch. There is no replay and therefore no need for keyframes. Trimming
// the stack's oldest entry is also free: entry 0's patch only ever rebuilt entry 0 from entry 1,
// so erasing it leaves every remaining link intact.
//
// How much this actually saves depends on the edit. A transform tweak or a rename is a handful
// of patch ops regardless of scene size (measured ~50x less retained text over a 100-step
// session). Inserting or deleting in the middle of a long array is the weak case: json::diff is
// index-based, so it rewrites every element after the insertion point, and such a patch can
// approach the size of the document itself. That's the pre-existing full-snapshot cost as the
// ceiling, never worse - a mixed session full of duplicates and deletes still measured ~10x
// smaller - so it isn't worth a smarter array diff.
//
// Correctness rests on nlohmann's documented round-trip guarantee:
//   source.patch(json::diff(source, target)) == target
// Everything here goes through parse/dump, so what round-trips is the parsed document - textual
// formatting never matters, and the scene loader re-parses anyway.
//
// `Entry` is any struct with a `std::string Delta` member (see EditorLayer::UndoEntry).
namespace UndoDelta {

// JSON Patch that turns `fromJson` into `toJson`. Returns "" if either side won't parse, which
// ApplyPatch treats as an unrecoverable link (a real empty patch dumps as "[]", never "").
inline std::string MakePatch(const std::string& fromJson, const std::string& toJson) {
    try {
        return nlohmann::json::diff(nlohmann::json::parse(fromJson),
                                    nlohmann::json::parse(toJson)).dump();
    } catch (const std::exception&) {
        return {};
    }
}

// Applies `patchJson` to `sourceJson`, writing the result to `out`. False (leaving `out`
// untouched) on any failure, including the "" sentinel above.
inline bool ApplyPatch(const std::string& sourceJson, const std::string& patchJson, std::string& out) {
    if (patchJson.empty()) return false;
    try {
        out = nlohmann::json::parse(sourceJson).patch(nlohmann::json::parse(patchJson)).dump();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Pushes `entry`, whose own full state is `newFullJson`. It becomes the stack's new top (held in
// full in `baseJson`) and the outgoing top is re-encoded as a patch off it.
template <class Entry>
void Push(std::vector<Entry>& stack, std::string& baseJson, Entry&& entry,
          const std::string& newFullJson) {
    if (!stack.empty()) {
        // The outgoing top's full state is `baseJson`; from here on it's reachable only as a
        // patch applied to the incoming top, which is the one now held in full.
        stack.back().Delta = MakePatch(newFullJson, baseJson);
    }
    entry.Delta.clear(); // the top entry is always the one held in full
    stack.push_back(std::move(entry));
    baseJson = newFullJson;
}

enum class PopResult {
    Empty,       // nothing was on the stack; outEntry/outFullJson untouched
    Ok,          // popped, and the entry underneath is now correctly anchored
    TailDropped  // popped correctly, but the chain below it was unreadable and got cleared
};

// Removes the top entry, handing back the entry itself and its full scene JSON, and re-anchors
// `baseJson` on the entry underneath.
template <class Entry>
PopResult Pop(std::vector<Entry>& stack, std::string& baseJson, Entry& outEntry,
              std::string& outFullJson) {
    if (stack.empty()) return PopResult::Empty;
    outFullJson = baseJson; // the top entry is the one held in full
    outEntry = std::move(stack.back());
    stack.pop_back();
    if (stack.empty()) {
        baseJson.clear();
        baseJson.shrink_to_fit();
        return PopResult::Ok;
    }
    // Re-anchor on the entry that's now on top by walking its patch back one step.
    if (!ApplyPatch(outFullJson, stack.back().Delta, baseJson)) {
        // Can't happen in practice (every link is built from our own serializer's output), but
        // if the chain ever did break, drop the unreachable tail rather than hand back a wrong
        // scene: the entry just popped still restores correctly, only the history behind it
        // is lost.
        stack.clear();
        baseJson.clear();
        return PopResult::TailDropped;
    }
    stack.back().Delta.clear(); // it's the top now, and the top is held in full
    return PopResult::Ok;
}

} // namespace UndoDelta
