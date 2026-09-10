#include "AtomicFile.h"
#include "Log.h"

#include <fstream>
#include <random>
#include <string>

namespace AtomicFile {

namespace {

// A per-write unique temp name in the target's own directory, so the final rename() is a
// same-filesystem move (atomic) rather than a cross-device copy. Concurrent writers to the same
// path (two editor instances) get different temps and the last rename wins — no interleaving.
std::filesystem::path TempPathFor(const std::filesystem::path& target) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::filesystem::path p = target;
    p += ".tmp-" + std::to_string(rng());
    return p;
}

} // namespace

bool WriteBytes(const std::filesystem::path& path, std::string_view bytes, bool binary) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        ec.clear(); // a pre-existing directory reports an error on some stdlibs; the open below is the real check
    }

    const std::filesystem::path temp = TempPathFor(path);
    {
        std::ios::openmode mode = std::ios::out | std::ios::trunc;
        if (binary) mode |= std::ios::binary;
        std::ofstream f(temp, mode);
        if (!f.is_open()) {
            Log::Error("AtomicFile: cannot open temp file for '" + path.string() + "'.");
            return false;
        }
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f.good()) {
            Log::Error("AtomicFile: write failed for '" + path.string() + "' (disk full?).");
            f.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        // Windows rename() fails if the destination exists; fall back to remove + rename. This
        // widens the crash window to that gap, but only on the platforms/filesystems that need
        // it, and only for the instant between the two calls.
        std::error_code ec2;
        std::filesystem::remove(path, ec2);
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            Log::Error("AtomicFile: rename into place failed for '" + path.string() + "': " + ec.message());
            std::filesystem::remove(temp, ec2);
            return false;
        }
    }
    return true;
}

bool WriteJson(const std::filesystem::path& path, const nlohmann::json& j, int indent) {
    return WriteBytes(path, j.dump(indent) + "\n", /*binary=*/false);
}

} // namespace AtomicFile
