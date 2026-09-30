#pragma once

// --perf-bench --perf-sample: a statistical CPU profiler for the main thread, for finding where
// time goes below the PROFILE_SCOPE level without an elevated ETW session. A background thread
// suspends the main thread about once a millisecond, unwinds its stack (x64 unwind tables, no
// allocation while it is suspended) and resumes it; Report() symbolizes the samples through the
// PDB and prints the functions with the most exclusive (on-CPU in the function itself) and
// inclusive (anywhere on the stack) samples. Windows x64 only; elsewhere every call is a no-op.
namespace SamplingProfiler {
    // Call from the thread to sample. Start() clears any previous samples.
    void Start();
    void Stop();
    // Prints "[PerfSample] <label> ..." lines to stdout: the top `top` functions both ways.
    void Report(const char* label, int top = 40);
}
