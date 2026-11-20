#pragma once

#include <string>
#include <string_view>
#include <vector>

// What loaded on the main thread between two calls of Take(): texture uploads, shader programs, model loads and the
// async loader's finish steps. --perf-bench turns it on and takes it once a frame, so each of its worst frames can say
// what was loaded during it. Off (the default), Note() is one relaxed atomic load.
namespace FrameLoads {

struct Event {
    const char* Kind;  // a string literal: "texture upload", "shader", "model load", ...
    std::string Name;  // the file (or the shader's debug name)
    double Ms;
};

void Enable(bool on);
bool Enabled();
// Records one load (any thread). `kind` must outlive the log: a string literal.
void Note(const char* kind, std::string_view name, double ms);
// Moves the loads since the last call into `out` (cleared first).
void Take(std::vector<Event>& out);
// One line for a frame's loads: per kind the count and total ms, with up to three names each.
std::string Summarize(const std::vector<Event>& events);

} // namespace FrameLoads
