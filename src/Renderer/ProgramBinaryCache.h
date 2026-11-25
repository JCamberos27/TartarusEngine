#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// The linked-program cache (project/Library/ShaderCache, git-ignored): the driver's binary of every GL program the
// engine links, kept on disk so the next run hands it back with glProgramBinary instead of compiling and linking the
// GLSL again. A first-run program costs tens to hundreds of ms (the big uber-shaders ~1 s); a cached one is a file
// read and a driver upload.
//
// An entry is keyed on the program's full source text (every stage, after the variant #defines are prepended) and
// stores the GL vendor / renderer / version string it was made with. A different source is a different entry; a
// different GPU or driver is a mismatch that is read as a miss. If the driver still refuses a binary (it may, after
// an update that keeps the version string), the program is compiled from source as before and the entry replaced.
// Nothing here can make a program fail that would have linked: every failure path falls back to a compile.
//
// Entries are written to a temporary file and renamed into place, so parallel engines (worktrees, test instances)
// each see a whole entry or none. Off with TARTARUS_NO_PROGRAM_CACHE=1 (A/B runs), and off on a context without
// program binary formats.
namespace ProgramBinaryCache {

struct Key {
    std::uint64_t Source = 0;      // hash of every stage's source, stage-tagged
    std::uint64_t SourceBytes = 0; // total source length (a second check against a hash collision)
    bool Valid = false;            // false: this program isn't cached (cache off)
};

// --- Pure parts (no GL; unit-tested) ---
// The key of a program whose stages are `stages`, in attach order. Each stage's text is tagged with its index, so
// moving text from one stage to another is a different key.
Key MakeKeyFor(std::initializer_list<std::string_view> stages);
// An entry's bytes for a program binary `binary` of `format` made by `driver`.
std::string EncodeEntry(const Key& key, std::string_view driver, std::uint32_t format, std::string_view binary);
// Reads `bytes` back. False on any mismatch (magic, version, key, driver, length, checksum); then `format` and
// `binary` are not to be used.
bool DecodeEntry(std::string_view bytes, const Key& key, std::string_view driver, std::uint32_t& format, std::string& binary);

// --- GL side (call with the context current, i.e. from the render thread) ---
// Whether programs go through the cache: the env switch, the entry points, and at least one binary format.
bool Enabled();
// The key for these stages; invalid when the cache is off.
Key MakeKey(std::initializer_list<std::string_view> stages);
// A linked program made from the cached binary for `key`, or 0 on a miss / a refused binary (a refused entry is
// deleted). Counts a hit or a miss.
unsigned int TryLoad(const Key& key);
// Call on a new program before glLinkProgram when it will be stored: drivers may only hand back a binary they were
// asked to keep (GL_PROGRAM_BINARY_RETRIEVABLE_HINT).
void PrepareForStore(unsigned int program, const Key& key);
// Writes the linked `program`'s binary as the entry for `key`. Never throws; a failed write leaves no entry.
// `compileMs` is what building it from source took (for the stats).
void Store(const Key& key, unsigned int program, double compileMs);

// --- Folder upkeep ---
struct Usage {
    std::uint64_t Bytes = 0;
    int Entries = 0;
};
Usage DiskUsage();
int Clear();
// Removes entries unused for 30 days, abandoned temp files, and the least recently used entries past the size cap.
void Prune();
// The folder (tests point it at a scratch folder; "" = project/Library/ShaderCache).
void SetDirectoryForTests(const std::string& dir);
// Entry file I/O without GL, for the tests: write / read the entry for `key` with this driver string.
bool WriteEntryForTests(const Key& key, std::string_view driver, std::uint32_t format, std::string_view binary);
bool ReadEntryForTests(const Key& key, std::string_view driver, std::uint32_t& format, std::string& binary);

// Since start: programs made from a cached binary, programs compiled (cache on, no usable entry), binaries the
// driver refused, and the time each kind took (ms).
struct Stats {
    int Hits = 0, Compiled = 0, Refused = 0;
    double HitMs = 0.0, CompileMs = 0.0;
};
Stats GetStats();
// "Program cache: N programs read (M ms), K compiled and stored (L ms)" or "" when nothing went through it.
std::string Summary();

} // namespace ProgramBinaryCache
