#pragma once

// The one hot-reload lifecycle both module hosts share (#172 / #187): HotReloadGameModule
// (TartarusGame.dll) and HotReloadEditorModule (TartarusEditor.dll). Header-only because the two
// hosts' API structs are unrelated types that merely agree on these members:
//
//     bool        (*OnLoad)(const void* state, std::size_t stateSize);  // may be null
//     void        (*OnUnload)();                                        // may be null
//     std::size_t (*SaveState)(void* buffer, std::size_t capacity);     // may be null
//
// Contract, in order, once the caller has loaded and validated a candidate DLL:
//   1. The LIVE module's SaveState is asked for its size (buffer null), then fills a host buffer.
//      Whatever it writes is opaque to the host.
//   2. The live module's OnUnload runs, and its DLL is freed. Its private copy stays on disk.
//   3. The CANDIDATE's OnLoad runs with that state. The old module is gone by now, so it can never
//      tear down shared state the new one has just set up.
//   4. OnLoad returning true commits: the old copy is deleted. Returning false rejects the build:
//      the candidate must leave nothing registered (the host won't call its OnUnload), it's freed,
//      and the previous build is loaded again from its copy and handed the same state, so the
//      user keeps a working module and loses nothing. If even that fails, no module is live.
//
// A module that keeps no state leaves SaveState null and gets (nullptr, 0) in OnLoad.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

#include "Log.h"

namespace HotReloadSwap {

// A loaded module: the DLL handle, its API table, and the private copy it was loaded from.
template <class API>
struct Slot {
    HMODULE Handle = nullptr;
    const API* Api = nullptr;
    std::filesystem::path Copy;
    explicit operator bool() const { return Api != nullptr; }
};

enum class Result { Committed, RolledBack, Failed };

// Opaque state from the live module's SaveState (empty when it has none, or no module is live).
template <class API>
std::vector<unsigned char> CaptureState(const API* api) {
    std::vector<unsigned char> state;
    if (!api || !api->SaveState) return state;
    const std::size_t needed = api->SaveState(nullptr, 0);
    if (needed == 0) return state;
    state.resize(needed);
    const std::size_t written = api->SaveState(state.data(), state.size());
    state.resize(written <= needed ? written : 0); // a module that changed its mind mid-call gets nothing
    return state;
}

// Runs OnLoad; a module without one accepts unconditionally.
template <class API>
bool CallOnLoad(const API* api, const std::vector<unsigned char>& state) {
    if (!api->OnLoad) return true;
    return api->OnLoad(state.empty() ? nullptr : state.data(), state.size());
}

// Steps 1-4 above. `live` is replaced by whichever module ends up running (the candidate, the
// restored previous build, or nothing). `loadAPI` re-resolves and validates an API table from a
// freshly loaded HMODULE (the same check the caller applied to the candidate), used for the
// rollback. `label` prefixes log lines ("Hot reload", "Editor hot reload").
template <class API>
Result Swap(Slot<API>& live, Slot<API> candidate,
            const std::function<const API*(HMODULE)>& loadAPI, const std::string& label) {
    std::error_code ec;
    const std::vector<unsigned char> state = CaptureState(live.Api);

    Slot<API> previous = live;
    if (previous.Api && previous.Api->OnUnload) previous.Api->OnUnload();
    if (previous.Handle) ::FreeLibrary(previous.Handle);
    live = {};

    if (CallOnLoad(candidate.Api, state)) {
        if (!previous.Copy.empty()) std::filesystem::remove(previous.Copy, ec);
        live = candidate;
        return Result::Committed;
    }

    ::FreeLibrary(candidate.Handle);
    std::filesystem::remove(candidate.Copy, ec);
    if (previous.Copy.empty()) {
        Log::Error(label + ": the module's OnLoad rejected this build; no module is loaded.");
        return Result::Failed;
    }

    Log::Error(label + ": the rebuilt module's OnLoad rejected this build; restoring the previous one.");
    HMODULE restored = ::LoadLibraryW(previous.Copy.c_str());
    const API* restoredAPI = restored ? loadAPI(restored) : nullptr;
    if (restoredAPI && CallOnLoad(restoredAPI, state)) {
        live = Slot<API>{restored, restoredAPI, previous.Copy};
        return Result::RolledBack;
    }
    if (restored) ::FreeLibrary(restored);
    std::filesystem::remove(previous.Copy, ec);
    Log::Error(label + ": the previous build couldn't be restored either; no module is loaded until the next rebuild.");
    return Result::Failed;
}

// Final teardown: SaveState is skipped (nothing will restore it), OnUnload runs, DLL + copy go.
template <class API>
void Unload(Slot<API>& live) {
    if (live.Api && live.Api->OnUnload) live.Api->OnUnload();
    if (live.Handle) ::FreeLibrary(live.Handle);
    std::error_code ec;
    if (!live.Copy.empty()) std::filesystem::remove(live.Copy, ec);
    live = {};
}

} // namespace HotReloadSwap
