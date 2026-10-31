#pragma once
#include <cstddef>
#include <map>
#include <mutex>
#include <string>

// --mem-report: the render-target-like allocations (shadow maps, the HDR target, decal arrays...) note their size
// here when they (re)create, so MemReport can list them next to the driver's VRAM total. A key is overwritten on
// each recreate; Set(key, 0) clears one. Counters only - nothing here allocates GPU memory or alters rendering.
namespace GpuMemTrack {
inline std::mutex& Mu() { static std::mutex m; return m; }
inline std::map<std::string, size_t>& Entries() { static std::map<std::string, size_t> e; return e; }
inline void Set(const std::string& key, size_t bytes) {
    std::lock_guard<std::mutex> l(Mu());
    if (bytes == 0) Entries().erase(key); else Entries()[key] = bytes;
}
inline std::map<std::string, size_t> Snapshot() {
    std::lock_guard<std::mutex> l(Mu());
    return Entries();
}
}
