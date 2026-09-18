#include "ShaderLibrary.h"
#include "Log.h"
#include "ProjectPaths.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace ShaderLibrary {

static std::filesystem::path s_Dir;

// #158 - source-string numbers for #line. Stable for the process, so the same file always maps
// to the same number whichever shader includes it.
static std::vector<std::string> s_FileNames{"<source>"};

static int FileIndex(const std::string& name) {
    for (size_t i = 1; i < s_FileNames.size(); ++i)
        if (s_FileNames[i] == name) return (int)i;
    s_FileNames.push_back(name);
    return (int)s_FileNames.size() - 1;
}

static std::string Trimmed(const std::string& line) {
    size_t a = line.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = line.find_last_not_of(" \t\r");
    return line.substr(a, b - a + 1);
}

// Advances a /* ... */ state machine over one line. Returns whether the line STARTS inside a
// block comment (the state before it), and updates `inBlock` to the state after it.
static bool StepBlockComment(const std::string& line, bool& inBlock) {
    const bool startedInside = inBlock;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (!inBlock && line[i] == '/' && line[i + 1] == '/') break; // rest is a line comment
        if (!inBlock && line[i] == '/' && line[i + 1] == '*') { inBlock = true; ++i; }
        else if (inBlock && line[i] == '*' && line[i + 1] == '/') { inBlock = false; ++i; }
    }
    return startedInside;
}

// #158 - `#include "x"` only counts at the start of a line (after whitespace) and outside a block
// comment; before, "// #include" or a mention inside /* */ was expanded too. `#pragma once` is
// honoured per top-level ReadFile. Every include is bracketed with #line directives so compile
// errors map back to the file and line they came from.
static std::string ResolveIncludes(const std::string& src, const std::filesystem::path& dir, int depth,
                                   const std::string& fileName, std::set<std::string>& onceFiles) {
    if (depth > 8) {
        Log::Error("ShaderLibrary: #include nesting too deep");
        return src;
    }
    const int fileIdx = FileIndex(fileName);
    std::istringstream ss(src);
    std::ostringstream out;
    if (depth > 0) out << "#line 1 " << fileIdx << '\n';
    std::string line;
    int lineNo = 0;
    bool inBlock = false;
    while (std::getline(ss, line)) {
        ++lineNo;
        const bool commented = StepBlockComment(line, inBlock);
        const std::string t = commented ? std::string() : Trimmed(line);
        if (t == "#pragma once") {
            onceFiles.insert(fileName);
            out << '\n'; // keep the line count
            continue;
        }
        if (depth == 0 && t.rfind("#version", 0) == 0) {
            // #line may not precede #version, so the top-level file's numbering starts after it.
            out << line << '\n' << "#line " << (lineNo + 1) << ' ' << fileIdx << '\n';
            continue;
        }
        if (t.rfind("#include", 0) == 0) {
            size_t q1 = t.find('"', 8);
            size_t q2 = (q1 != std::string::npos) ? t.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string name = t.substr(q1 + 1, q2 - q1 - 1);
                if (onceFiles.count(name)) { out << '\n'; continue; } // already included, #pragma once
                // #208 — next to the including file first (a project shader's own includes),
                // then the engine shader directory (its shared Lighting.glsl etc.).
                std::filesystem::path incPath = dir / name;
                std::error_code existsEc;
                if (!std::filesystem::exists(incPath, existsEc) && !s_Dir.empty() && dir != s_Dir)
                    incPath = s_Dir / name;
                std::ifstream f(incPath);
                if (!f.is_open()) {
                    Log::Error("ShaderLibrary: cannot open include \"" + name + "\" (looked in " + dir.string() +
                               (dir != s_Dir ? " and " + s_Dir.string() : std::string()) +
                               ") from " + fileName + ":" + std::to_string(lineNo));
                    out << line << '\n';
                } else {
                    std::ostringstream content;
                    content << f.rdbuf();
                    out << ResolveIncludes(content.str(), incPath.parent_path(), depth + 1, name, onceFiles);
                    out << "#line " << (lineNo + 1) << ' ' << fileIdx << '\n';
                }
                continue;
            }
        }
        out << line << '\n';
    }
    return out.str();
}

std::string AnnotateLog(const std::string& log) {
    // NVIDIA: "3(12) : error ..."; AMD / Intel / Mesa: "ERROR: 3:12: ...".
    static const std::regex kNv(R"((^|\n)(\d+)\((\d+)\))");
    static const std::regex kOther(R"((ERROR|WARNING): (\d+):(\d+):)");
    auto name = [](const std::string& idx) {
        const size_t i = (size_t)std::stoul(idx);
        return i < s_FileNames.size() ? s_FileNames[i] : idx;
    };
    std::string out;
    std::smatch m;
    std::string rest = log;
    while (std::regex_search(rest, m, kNv)) {
        out += m.prefix().str() + m[1].str() + name(m[2].str()) + "(" + m[3].str() + ")";
        rest = m.suffix().str();
    }
    out += rest;
    rest = out;
    out.clear();
    while (std::regex_search(rest, m, kOther)) {
        out += m.prefix().str() + m[1].str() + ": " + name(m[2].str()) + ":" + m[3].str() + ":";
        rest = m.suffix().str();
    }
    return out + rest;
}

void Init(const std::string& shadersDir) {
    s_Dir = shadersDir;
}

std::string Dir() {
    return s_Dir.string();
}

std::string ReadFileRequired(const std::string& filename) {
    std::filesystem::path path = s_Dir / filename;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        throw std::runtime_error(
            "Required engine shader not found: " + path.string() +
            "\n(shader directory resolved to: " + (s_Dir.empty() ? std::string("<unset>") : s_Dir.string()) +
            ")\nLaunch the executable from its output directory, or set TARTARUS_ASSET_ROOT.");
    }
    std::string src = ReadFile(filename);
    if (src.empty()) {
        throw std::runtime_error("Required engine shader is empty or unreadable: " + path.string());
    }
    return src;
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
    std::set<std::string> onceFiles;
    return ResolveIncludes(ss.str(), s_Dir, 0, filename, onceFiles);
}

std::string ReadFileAt(const std::string& pathStr, const std::string& referencedBy) {
    const std::filesystem::path path(pathStr);
    std::ifstream f(path);
    if (!f.is_open()) {
        Log::Error("ShaderLibrary: cannot open shader: " + path.string() +
                   (referencedBy.empty() ? std::string() : " (referenced by " + referencedBy + ")"),
                   LogContext::Asset(referencedBy.empty() ? pathStr : referencedBy));
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::set<std::string> onceFiles;
    return ResolveIncludes(ss.str(), path.parent_path(), 0, path.filename().string(), onceFiles);
}

std::string ResolveRef(const std::string& ref, const std::string& baseDir) {
    auto startsWith = [&](const char* prefix) { return ref.rfind(prefix, 0) == 0; };
    if (startsWith("engine://")) return (s_Dir / ref.substr(9)).string();
    if (startsWith("project://")) return ProjectPaths::Resolve(ref.substr(10));
    const std::filesystem::path p(ref);
    if (p.is_absolute()) return p.string();
    std::error_code ec;
    if (!baseDir.empty()) {
        const std::filesystem::path local = std::filesystem::path(baseDir) / p;
        if (std::filesystem::exists(local, ec)) return local.string();
    }
    const std::filesystem::path engine = s_Dir / p;
    if (std::filesystem::exists(engine, ec)) return engine.string();
    // Neither exists: report the descriptor-relative location, the one the author meant.
    return baseDir.empty() ? engine.string() : (std::filesystem::path(baseDir) / p).string();
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
