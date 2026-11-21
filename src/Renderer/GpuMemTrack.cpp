#include "GpuMemTrack.h"

std::mutex& GpuMemTrack::Mu() { static std::mutex* m = new std::mutex; return *m; }
std::map<std::string, size_t>& GpuMemTrack::Entries() { static auto* e = new std::map<std::string, size_t>; return *e; }
