#include "ShaderLibrary.h"
#include "Log.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <unordered_map>

namespace ShaderLibrary {

static std::filesystem::path s_Dir;

static std::string ResolveIncludes(const std::string& src, const std::filesystem::path& dir, int depth) {
    if (depth > 8) {
        Log::Error("ShaderLibrary: #include nesting too deep");
        return src;
    }
    std::istringstream ss(src);
    std::ostringstream out;
    std::string line;
    while (std::getline(ss, line)) {
        size_t inc = line.find("#include");
        if (inc != std::string::npos) {
            size_t q1 = line.find('"', inc + 8);
            size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string name = line.substr(q1 + 1, q2 - q1 - 1);
                std::ifstream f(dir / name);
                if (!f.is_open()) {
                    Log::Error("ShaderLibrary: cannot open include: " + (dir / name).string());
                    out << line << '\n';
                } else {
                    std::ostringstream content;
                    content << f.rdbuf();
                    out << ResolveIncludes(content.str(), dir, depth + 1);
                }
                continue;
            }
        }
        out << line << '\n';
    }
    return out.str();
}

void Init(const std::string& shadersDir) {
    s_Dir = shadersDir;
}

std::string ReadFile(const std::string& filename) {
    std::filesystem::path path = s_Dir / filename;
    std::ifstream f(path);
    if (!f.is_open()) {
        Log::Error("ShaderLibrary: cannot open shader: " + path.string());
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ResolveIncludes(ss.str(), s_Dir, 0);
}

void PollForChanges(const std::function<void(const std::string& filename)>& onChanged) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_LastPoll;
    static std::unordered_map<std::string, std::filesystem::file_time_type> s_MTimes;

    constexpr auto kInterval = std::chrono::milliseconds(250); // ~4 Hz
    auto now = Clock::now();
    if (now - s_LastPoll < kInterval) return;
    s_LastPoll = now;

    if (s_Dir.empty()) return;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(s_Dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".glsl" && ext != ".vert" && ext != ".frag" && ext != ".comp") continue;

        auto mtime = entry.last_write_time(ec);
        if (ec) continue;
        std::string name = entry.path().filename().string();
        auto it = s_MTimes.find(name);
        if (it == s_MTimes.end()) {
            s_MTimes[name] = mtime; // first seen — no notification
        } else if (it->second != mtime) {
            it->second = mtime;
            onChanged(name);
        }
    }
}

} // namespace ShaderLibrary
