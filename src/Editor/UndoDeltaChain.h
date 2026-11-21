#pragma once
#include <json.hpp>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
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
// `Entry` is any struct with an `UndoDelta::Patch Delta` member (see EditorLayer::UndoEntry).
//
// The patch is worked out on a background thread (Patch::Start): the diff of two ~1 MB scenes
// took ~27 ms, on every undoable edit, on the main thread. Pop - the only reader - waits for it,
// and in practice it's long finished by then.
namespace UndoDelta {

// Cheap non-cryptographic FNV-1a hash of a serialized scene snapshot. Only ever compared with
// another snapshot's hash to tell "the same scene" from "a different one", never used to rebuild
// anything. Same constants and style as TextureCache::HashSettings.
inline uint64_t HashText(const std::string& s) {
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull; // FNV-1a prime
    }
    return h;
}

// JSON Patch that turns `fromJson` into `toJson`. Returns "" if either side won't parse, which
// ApplyPatch treats as an unrecoverable link (a real empty patch dumps as "[]", never "").
inline std::string MakePatch(const std::string& fromJson, const std::string& toJson) {
    if (fromJson == toJson) return "[]"; // #138 - skip two parses + a diff for an unchanged scene
    try {
        return nlohmann::json::diff(nlohmann::json::parse(fromJson),
                                    nlohmann::json::parse(toJson)).dump();
    } catch (const std::exception&) {
        return {};
    }
}

// Applies `patchJson` to `sourceJson`, writing the result to `out`. False (leaving `out`
// untouched) on any failure, including the "" sentinel above.
// A JSON Patch's text, possibly still being worked out on a background thread.
class Patch {
public:
    Patch() = default;
    Patch(std::string text) : m_Text(std::move(text)) {}

    // Diffs `fromJson` -> `toJson` on a background thread. The strings are copied in, so the
    // caller's may change or go straight away.
    void Start(std::string fromJson, std::string toJson) {
        m_Text.clear();
        auto promise = std::make_shared<std::promise<std::string>>();
        m_Job = promise->get_future().share();
        // Detached, with a promise rather than std::async: dropping an entry (history trimmed,
        // redo cleared) then never blocks on a diff still running.
        std::thread([promise, from = std::move(fromJson), to = std::move(toJson)] {
            promise->set_value(MakePatch(from, to));
        }).detach();
    }
    // The patch text (waiting for a background diff to finish).
    const std::string& Text() const {
        if (m_Job.valid()) {
            m_Text = m_Job.get();
            m_Job = {};
        }
        return m_Text;
    }
    bool empty() const { return !m_Job.valid() && m_Text.empty(); }
    void clear() {
        m_Text.clear();
        m_Job = {};
    }

private:
    mutable std::string m_Text;
    mutable std::shared_future<std::string> m_Job;
};

inline bool ApplyPatch(const nlohmann::json& source, const std::string& patchJson, std::string& out) {
    if (patchJson.empty()) return false;
    try {
        out = source.patch(nlohmann::json::parse(patchJson)).dump();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
inline bool ApplyPatch(const std::string& sourceJson, const std::string& patchJson, std::string& out) {
    if (patchJson.empty()) return false;
    try {
        return ApplyPatch(nlohmann::json::parse(sourceJson), patchJson, out);
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
        if (newFullJson == baseJson) stack.back().Delta = Patch("[]");
        else stack.back().Delta.Start(newFullJson, baseJson);
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
// `baseJson` on the entry underneath. `outParsed`, when given, also gets `outFullJson` parsed (the
// re-anchoring parses it anyway), or a discarded value when it won't parse.
template <class Entry>
PopResult Pop(std::vector<Entry>& stack, std::string& baseJson, Entry& outEntry,
              std::string& outFullJson, nlohmann::json* outParsed = nullptr) {
    if (stack.empty()) return PopResult::Empty;
    outFullJson = baseJson; // the top entry is the one held in full
    outEntry = std::move(stack.back());
    stack.pop_back();
    nlohmann::json parsed(nlohmann::json::value_t::discarded);
    if (outParsed || !stack.empty()) {
        try { parsed = nlohmann::json::parse(outFullJson); }
        catch (const std::exception&) { parsed = nlohmann::json(nlohmann::json::value_t::discarded); }
    }
    struct HandOver { nlohmann::json& From; nlohmann::json* To; ~HandOver() { if (To) *To = std::move(From); } } handOver{parsed, outParsed};
    if (stack.empty()) {
        baseJson.clear();
        baseJson.shrink_to_fit();
        return PopResult::Ok;
    }
    // Re-anchor on the entry that's now on top by walking its patch back one step.
    if (parsed.is_discarded() || !ApplyPatch(parsed, stack.back().Delta.Text(), baseJson)) {
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

// A stack's base (its top entry's full state) that a deferred Pop may still be working out on a
// background thread. Every read waits for that first, so a caller can never see the old top's
// text in its place. Empty when the stack is empty, or after a re-anchor that failed (TakeBroken).
class Base {
public:
    std::string& Text() { Settle(); return m_Text; }
    const std::string& Text() const { Settle(); return m_Text; }
    bool empty() const { return Text().empty(); }
    void clear() {
        m_Job = {};
        m_Text.clear();
        m_Text.shrink_to_fit();
        m_Broken = false;
    }
    // True once if a deferred re-anchor failed: the entries under the top can't be rebuilt.
    bool TakeBroken() {
        Settle();
        const bool broken = m_Broken;
        m_Broken = false;
        return broken;
    }
    // The base becomes `source` with `patch` applied, worked out on a background thread.
    void Defer(std::string source, std::string patch) {
        m_Text.clear();
        m_Broken = false;
        auto promise = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        m_Job = promise->get_future();
        std::thread([promise, source = std::move(source), patch = std::move(patch)] {
            std::string out;
            const bool ok = ApplyPatch(source, patch, out);
            promise->set_value({ok, ok ? std::move(out) : std::string()});
        }).detach();
    }

private:
    void Settle() const {
        if (!m_Job.valid()) return;
        auto [ok, text] = m_Job.get();
        m_Text = std::move(text);
        m_Broken = !ok;
    }
    mutable std::string m_Text;
    mutable bool m_Broken = false;
    mutable std::future<std::pair<bool, std::string>> m_Job;
};

// Pop for a caller that doesn't need the popped state parsed (an in-place undo, X-33 C): hands
// back the top entry and its full text at once, and re-anchors `base` on the entry underneath on
// a background thread instead of parsing, patching and dumping the whole scene here.
template <class Entry>
PopResult PopDeferred(std::vector<Entry>& stack, Base& base, Entry& outEntry, std::string& outFullJson) {
    if (stack.empty()) return PopResult::Empty;
    outFullJson = base.Text();
    outEntry = std::move(stack.back());
    stack.pop_back();
    if (stack.empty()) {
        base.clear();
        return PopResult::Ok;
    }
    std::string patch = stack.back().Delta.Text(); // waits for its diff, normally long done
    stack.back().Delta.clear();                    // it's the top now, and the top is held in full
    base.Defer(outFullJson, std::move(patch));
    return PopResult::Ok;
}

} // namespace UndoDelta
