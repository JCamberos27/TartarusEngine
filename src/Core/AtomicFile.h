#pragma once

#include <filesystem>
#include <string_view>
#include <json.hpp>

// Crash-safe file writes: serialize to a sibling temp file, flush it to disk (FlushFileBuffers on
// Windows), then atomically replace the target (MoveFileExW REPLACE_EXISTING — the target is never
// deleted first, #92). rename() within a directory is atomic on every filesystem this engine targets, so
// a crash, power loss, or full disk mid-write can never leave the real file half-written and
// readable as valid — a reader sees either the old bytes or the new bytes, never a truncation
// (audit CPP-206). Matches TextureCache::Store, which already does this for the .ttex cache.
//
// Use for every engine-written .json / .meta / .mat / prefs / scene / prefab file.
namespace AtomicFile {

// Writes `bytes` to `path` atomically. `binary` opens the temp stream in binary mode (no CRLF
// translation) — pass true for anything that must round-trip byte-exact. Creates parent
// directories. Returns false (and leaves any existing file untouched) on any I/O error.
bool WriteBytes(const std::filesystem::path& path, std::string_view bytes, bool binary = false);

// Convenience: dump `j` (pretty-printed with `indent` spaces, trailing newline) and write it
// atomically via WriteBytes.
bool WriteJson(const std::filesystem::path& path, const nlohmann::json& j, int indent = 2);

} // namespace AtomicFile
