#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// What else the machine was doing while --perf-bench measured a phase: the CPU time every other process used and the
// engines / compilers / encoders running at the phase's start and end. A phase that shared the machine reads
// CONTENDED, so its hitches aren't taken for engine work (another engine shares the GPU, a build preempts the CPU and
// the stalls land in whatever scope was running). Two snapshots per phase, none per frame: the process list costs a
// few ms and must not make hitches of its own.
namespace MachineLoad {

struct Snapshot {
    bool Ok = false;
    uint64_t SysBusy = 0, SysTotal = 0; // every core's busy / total time (100 ns units)
    uint64_t SelfCpu = 0;               // this process's kernel + user time
    struct Proc { std::string Name; uint32_t Pid = 0; uint64_t Cpu = 0; bool CpuKnown = false; };
    std::vector<Proc> Procs; // watched processes (this one left out) with their kernel + user time so far
};

struct Result {
    bool Ok = false;
    double OtherCpuPct = 0.0;  // share of all cores used by other processes between the snapshots
    std::string Busy;          // "TartarusEngine x1, cl x8": watched processes that took CPU during the phase, or ""
    std::string Idle;          // the watched ones that sat idle (MSBuild's reused nodes, an idle editor), or ""
    bool Contended = false;
};

// Other processes' CPU share at or above this marks the phase contended.
constexpr double kContendedCpuPct = 25.0;
// A watched process counts as busy above this share of ONE core over the phase, or when it started during the phase.
constexpr double kBusyCorePct = 5.0;

Snapshot Take();
// Pure: the load between two snapshots (unit-tested on hand-made ones).
Result Between(const Snapshot& start, const Snapshot& end, int cores);
// "other CPU 31% (12 cores); busy: cl x8; idle: MSBuild x6 -> CONTENDED", or "" when the snapshots failed.
std::string Describe(const Result& r, int cores);
int Cores();

} // namespace MachineLoad
