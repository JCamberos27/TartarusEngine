#pragma once

#include <cstdint>
#include <string>
#include <vector>

// DIAG (G-MB part 1): what splits the opaque multi-draw batches. Each opaque item's batch key is rebuilt from its parts;
// per view we count the distinct full keys (= batches), each part's distinct values, and the batches left if a part
// (or a set of parts) left the key. Gated by TARTARUS_BATCH_CENSUS=1; printed per --perf-bench phase.
namespace BatchCensus {

enum Part : int { Probe, GI, ObjFlags, Program, Material, Textures, State, Page, kParts };

struct Item {
    std::uint64_t P[kParts];
};

bool Enabled();
// One view's opaque items (BuildBatches). batches = what BuildIndirectBatches made (cross-check).
void Record(const std::vector<Item>& items, std::size_t batches);
// Prints the phase's averages per view and resets.
void Report(const std::string& scene, const char* phase);

} // namespace BatchCensus
