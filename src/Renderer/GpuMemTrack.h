#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

// --mem-report: the render-target-like allocations (shadow maps, the HDR target, decal arrays...) note their size
// here when they (re)create, so MemReport can list them next to the driver's VRAM total. A key is overwritten on
// each recreate; Set(key, 0) clears one. Counters only - nothing here allocates GPU memory or alters rendering.
namespace GpuMemTrack {
// Never destroyed: render targets owned by statics call Set from their destructors at exit. Out of line
// (GpuMemTrack.cpp) so every module shares one list: an inline function's static is one copy per DLL.
std::mutex& Mu();
std::map<std::string, size_t>& Entries();
inline void Set(const std::string& key, size_t bytes) {
    std::lock_guard<std::mutex> l(Mu());
    if (bytes == 0) Entries().erase(key); else Entries()[key] = bytes;
}
// A key unique to one owner (several instances of the same target type can be alive at once) and stable across
// its resizes. Clear it with Set(key, 0) when the owner releases.
inline std::string Key(const std::string& name, const void* owner) {
    return name + " @" + std::to_string(reinterpret_cast<std::uintptr_t>(owner));
}
inline std::map<std::string, size_t> Snapshot() {
    std::lock_guard<std::mutex> l(Mu());
    return Entries();
}
}
