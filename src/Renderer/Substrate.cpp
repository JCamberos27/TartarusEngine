#include "Substrate.h"
#include "SubstrateJson.h"
#include "Core/Paths.h"
#include "Log.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace {
std::mutex s_Mutex;
std::unordered_map<std::string, SubstrateDesc>& Cache() {
    static std::unordered_map<std::string, SubstrateDesc> c;
    return c;
}
} // namespace

namespace SubstratePresets {

SubstrateDesc Get(const std::string& path) {
    std::string key = path;
    for (char& c : key) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        if (auto it = Cache().find(key); it != Cache().end()) return it->second;
    }
    // Read and parsed unlocked: two threads asking for the same new preset both read it, and the first in wins.
    SubstrateDesc d;
    const std::string abs = path.empty() || std::filesystem::u8path(path).is_absolute() ? path : ProjectPaths::Resolve(path);
    std::ifstream f(std::filesystem::u8path(abs));
    if (f.is_open()) {
        try {
            const nlohmann::json j = nlohmann::json::parse(f);
            if (j.is_object()) SubstrateJson::ReadFields(j, d, SubstrateJson::PlainPath);
        } catch (const std::exception& e) {
            Log::Warn("Substrate: '" + path + "' isn't valid JSON (" + e.what() + ") - no substrate.");
            d = SubstrateDesc{};
        }
    } else if (!path.empty()) {
        Log::Warn("Substrate: preset '" + path + "' not found - no substrate.");
    }
    d.Preset = path;
    std::lock_guard<std::mutex> lock(s_Mutex);
    return Cache().emplace(key, std::move(d)).first->second;
}

void ClearCache() {
    std::lock_guard<std::mutex> lock(s_Mutex);
    Cache().clear();
}

} // namespace SubstratePresets
