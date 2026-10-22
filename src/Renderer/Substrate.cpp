#include "Substrate.h"
#include "SubstrateJson.h"
#include "ProjectPaths.h"
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

const SubstrateDesc& Get(const std::string& path) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    auto& cache = Cache();
    std::string key = path;
    for (char& c : key) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
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
    return cache.emplace(key, d).first->second;
}

void ClearCache() {
    std::lock_guard<std::mutex> lock(s_Mutex);
    Cache().clear();
}

} // namespace SubstratePresets
