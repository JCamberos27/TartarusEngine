#include "BatchCensus.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace BatchCensus {
namespace {

constexpr const char* kNames[kParts] = {"probe", "gi", "objflags", "program", "material", "textures", "state", "page"};

// Key variants: the full key, each part dropped alone, then the G-MB stages.
struct Variant {
    const char* Name;
    std::uint32_t Drop; // bit per Part
};
constexpr std::uint32_t B(Part p) { return 1u << p; }
const Variant kVariants[] = {
    {"full", 0},
    {"-probe", B(Probe)},
    {"-gi", B(GI)},
    {"-objflags", B(ObjFlags)},
    {"-program", B(Program)},
    {"-material", B(Material)},
    {"-textures", B(Textures)},
    {"-state", B(State)},
    {"-page", B(Page)},
    {"-probe-gi (G-MB 2)", B(Probe) | B(GI)},
    {"-probe-gi-material (G-MB 2+3)", B(Probe) | B(GI) | B(Material)},
    {"-probe-gi-material-textures (2+3+4)", B(Probe) | B(GI) | B(Material) | B(Textures)},
    {"program+page+state only", B(Probe) | B(GI) | B(ObjFlags) | B(Material) | B(Textures)},
    {"program+page only", B(Probe) | B(GI) | B(ObjFlags) | B(Material) | B(Textures) | B(State)},
};
constexpr int kVariantCount = (int)(sizeof(kVariants) / sizeof(kVariants[0]));

struct Acc {
    double Sum = 0;
    std::uint64_t Max = 0;
    void Add(std::uint64_t v) { Sum += (double)v; Max = std::max(Max, v); }
};
struct State {
    std::uint64_t Views = 0, Mismatch = 0;
    Acc Items, Built;
    Acc Distinct[kParts];
    Acc Batches[kVariantCount];
} g;

std::uint64_t Mix(std::uint64_t key, std::uint64_t v) { return key ^ (v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2)); }

} // namespace

bool Enabled() {
    static const bool on = [] { const char* v = std::getenv("TARTARUS_BATCH_CENSUS"); return v && *v && *v != '0'; }();
    return on;
}

void Record(const std::vector<Item>& items, std::size_t batches) {
    if (items.empty()) return;
    static std::unordered_set<std::uint64_t> seen;
    ++g.Views;
    g.Items.Add(items.size());
    g.Built.Add(batches);
    for (int p = 0; p < kParts; ++p) {
        seen.clear();
        for (const Item& it : items) seen.insert(it.P[p]);
        g.Distinct[p].Add(seen.size());
    }
    for (int v = 0; v < kVariantCount; ++v) {
        seen.clear();
        for (const Item& it : items) {
            std::uint64_t k = 0x2545F4914F6CDD1Dull;
            for (int p = 0; p < kParts; ++p) k = Mix(k, (kVariants[v].Drop >> p) & 1u ? 0x51ull + p : it.P[p]);
            seen.insert(k);
        }
        g.Batches[v].Add(seen.size());
        if (v == 0 && seen.size() != batches) ++g.Mismatch;
    }
}

void Report(const std::string& scene, const char* phase) {
    if (!Enabled()) return;
    const double n = g.Views ? (double)g.Views : 1.0;
    char line[256];
    std::snprintf(line, sizeof line, "[BatchCensus] %s (%s): views %llu, opaque items/view %.1f (max %llu), built batches/view %.1f (max %llu), full-key mismatches %llu",
                  scene.c_str(), phase, (unsigned long long)g.Views, g.Items.Sum / n, (unsigned long long)g.Items.Max,
                  g.Built.Sum / n, (unsigned long long)g.Built.Max, (unsigned long long)g.Mismatch);
    std::cout << line << '\n';
    for (int p = 0; p < kParts; ++p) {
        std::snprintf(line, sizeof line, "[BatchCensus] %s (%s)   distinct %-10s %8.1f /view (max %llu)", scene.c_str(), phase,
                      kNames[p], g.Distinct[p].Sum / n, (unsigned long long)g.Distinct[p].Max);
        std::cout << line << '\n';
    }
    for (int v = 0; v < kVariantCount; ++v) {
        std::snprintf(line, sizeof line, "[BatchCensus] %s (%s)   batches %-38s %8.1f /view (max %llu)", scene.c_str(), phase,
                      kVariants[v].Name, g.Batches[v].Sum / n, (unsigned long long)g.Batches[v].Max);
        std::cout << line << '\n';
    }
    std::cout.flush();
    g = State{};
}

} // namespace BatchCensus
