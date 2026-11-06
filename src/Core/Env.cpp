#include "Core/Env.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace Env {
namespace {

const Info kTable[] = {
#define TARTARUS_ENV_INFO(id, name, kind, help) {name, Kind::kind, help},
    TARTARUS_ENV_VARS(TARTARUS_ENV_INFO)
#undef TARTARUS_ENV_INFO
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == (size_t)Var::Count, "one row per Env::Var");

// The value, and whether it is set. _dupenv_s: MSVC's thread-safe form of getenv.
bool Read(Var v, std::string& out) {
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, Describe(v).Name) != 0 || !buf) return false;
    out = buf;
    std::free(buf);
    return true;
}

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Flag: return "flag";
        case Kind::Int: return "int";
        case Kind::Float: return "float";
        case Kind::Str: return "text";
        case Kind::List: return "list";
    }
    return "";
}

} // namespace

const Info& Describe(Var v) { return kTable[(size_t)v]; }

bool IsSet(Var v) {
    std::string s;
    return Read(v, s);
}

std::string Str(Var v, const std::string& fallback) {
    std::string s;
    return Read(v, s) ? s : fallback;
}

bool Flag(Var v) {
    std::string s;
    return Read(v, s) && !s.empty() && s != "0";
}

int Int(Var v, int fallback) {
    std::string s;
    if (!Read(v, s) || s.empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    const long n = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() || errno == ERANGE ? fallback : (int)n;
}

float Float(Var v, float fallback) {
    std::string s;
    if (!Read(v, s) || s.empty()) return fallback;
    char* end = nullptr;
    const float f = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : f;
}

void Set(Var v, const char* value) { _putenv_s(Describe(v).Name, value ? value : ""); }

std::string HelpText() {
    std::string out = "Environment variables (flags are on when set to anything but 0):\n";
    for (const Info& i : kTable) {
        std::string line = "  ";
        line += i.Name;
        line += " (";
        line += KindName(i.Type);
        line += ")";
        if (line.size() < 44) line.append(44 - line.size(), ' ');
        else line += ' ';
        out += line + i.Help + "\n";
    }
    return out;
}

} // namespace Env
