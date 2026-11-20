#pragma once

// The hot-reload hosts (HotReloadGameModule, HotReloadEditorModule) load a numbered copy of their DLL from a shared
// %TEMP% folder, named <prefix><pid>_<generation>.dll. At startup each sweeps copies orphaned by a crash or
// force-kill. A copy whose owner process is still running is left alone: another instance that started in the same
// moment may have copied it and not loaded (locked) it yet, and deleting it then makes that instance's load fail.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ModuleCopies {

// The owner pid in "<prefix><pid>_<generation>.dll"; nullopt when the name doesn't have that shape.
std::optional<std::uint32_t> OwnerPid(const std::wstring& fileName, const std::wstring& prefix);

// Removes every <prefix>*.dll in dir except those whose owner is a running process (or this one).
void SweepOrphans(const std::filesystem::path& dir, const std::wstring& prefix);

} // namespace ModuleCopies
