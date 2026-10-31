#pragma once
#include <string>
#include <vector>

class Model;

// --mem-report: TARTARUS_MEM_REPORT=<dir> makes the smoke harness write <dir>/<label>.json at the end of each
// scene: process RAM, the driver's VRAM use (NVIDIA), every live texture (size, format, GPU bytes) and every
// mesh (GPU and CPU bytes), with the file hash of each texture so duplicates show up. A measuring tool only.
namespace MemReport {
void Write(const std::string& dir, const std::string& label, const std::vector<const Model*>& models);
}
