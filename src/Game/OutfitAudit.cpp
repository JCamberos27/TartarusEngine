#include "OutfitAudit.h"

#include "AssetLibrary.h"
#include "Model.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "ProjectPaths.h"
#include "Wardrobe.h"

#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <thread>

namespace OutfitAudit {
namespace {

struct Candidate {
    std::string Slot, Path;
    bool BodyPart = false;
};

struct Pair {
    Wardrobe::Gender Sex;
    const Candidate* Under;
    const Candidate* Over;
    int Verts = 0, Hidden = 0, Clipping = 0;
    float Depth = 0.0f; // deepest vertex still poking through, metres
};

constexpr float kMaxDepth = 0.15f;    // how far out a poke is looked for
constexpr float kMinDepth = 0.002f;  // closer than this is the same surface, not a poke

// What's wrong with a resolved outfit: pieces the rules had to take off (the request asked for things
// that don't go together), style clashes left on, a variant asked for directly, and no top, pants or shoes.
std::vector<std::string> Problems(const OutfitSystem::Catalog& cat, const Wardrobe::Request& req,
                                  const Wardrobe::Resolved& r, bool randomized) {
    std::vector<std::string> out;
    for (const auto& d : r.Dropped) out.push_back("dropped " + d);
    for (const auto& c : r.Clashes) out.push_back("clash: " + c);
    auto worn = [&](const std::string& slot) -> const Wardrobe::Item* {
        for (const auto& p : r.Pieces)
            if (p.Slot == slot) return cat.Find(p.Path);
        return nullptr;
    };
    if (randomized) {
        for (const auto& [slot, path] : req.Items)
            if (const auto* it = cat.Find(path); it && it->Variant) out.push_back("picked variant " + it->Stem);
        const auto* outer = worn("Outerwear");
        const bool coversChest = outer && (Wardrobe::HasTag(*outer, "BuiltInTop") || Wardrobe::HasTag(*outer, "ClosedJacket"));
        if (!worn("Top") && !coversChest) out.push_back("no top");
        if (!worn("Pants")) out.push_back("no pants");
        if (!worn("Shoes")) out.push_back("no shoes");
    }
    return out;
}

std::string Describe(const Wardrobe::Request& req) {
    std::string s = std::string(Wardrobe::GenderName(req.Sex));
    for (const auto& [slot, path] : req.Items) s += " " + slot + "=" + Wardrobe::PrettyName(Wardrobe::Stem(path));
    return s;
}

// The rules against the artist's presets and against Randomize. Returns the outfits with problems.
int CheckRules(const OutfitSystem::Catalog& cat, int seeds) {
    const Wardrobe::Wardrobe& w = cat.W;
    int bad = 0;

    // 1. Every artist preset (assets/Characters/Outfits/Quantum) goes through the rules unchanged.
    int presets = 0, presetBad = 0;
    std::error_code ec;
    const auto dir = std::filesystem::path(ProjectPaths::Resolve("assets/Characters/Outfits"));
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".outfit") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        nlohmann::json j;
        try { std::ifstream(f) >> j; } catch (...) { continue; }
        Wardrobe::Request req;
        req.Sex = j.value("gender", std::string("Male")) == "Female" ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
        req.Race = j.value("race", std::string());
        if (j.contains("items"))
            for (const auto& [slot, v] : j["items"].items()) req.Items[slot] = v.value("item", std::string());
        ++presets;
        const auto r = Wardrobe::Resolve(w, cat.Items, req);
        auto problems = Problems(cat, req, r, false);
        for (const auto& [slot, path] : req.Items)
            if (!cat.Find(path)) problems.push_back("unknown item " + path);
        if (problems.empty()) continue;
        ++presetBad;
        std::cout << "  preset " << f.stem().string() << ":";
        for (const auto& p : problems) std::cout << " [" << p << "]";
        std::cout << "\n";
    }
    std::cout << "[OutfitAudit] presets: " << presets - presetBad << " of " << presets << " go through the rules unchanged\n";
    bad += presetBad;

    // 2. Randomize, `seeds` outfits per gender.
    for (int g = 0; g < 2; ++g) {
        Wardrobe::Request base;
        base.Sex = (Wardrobe::Gender)g;
        if (w.Body(base.Sex).Parts.empty()) continue;
        std::map<std::string, int> styles, fills, problemKinds;
        int randomBad = 0, shown = 0;
        for (int seed = 1; seed <= seeds; ++seed) {
            std::string style;
            const auto req = Wardrobe::Randomize(w, cat.Items, base, (unsigned)seed, {}, "", &style);
            ++styles[style];
            const auto r = Wardrobe::Resolve(w, cat.Items, req);
            for (const auto& p : r.Pieces) if (!p.BodyPart) ++fills[style + "/" + p.Slot];
            const auto problems = Problems(cat, req, r, true);
            if (problems.empty()) continue;
            ++randomBad;
            for (const auto& p : problems) ++problemKinds[p.substr(0, p.find(' ', p.find(' ') + 1))];
            if (shown++ < 12) {
                std::cout << "  seed " << seed << " (" << style << ") " << Describe(req) << ":";
                for (const auto& p : problems) std::cout << " [" << p << "]";
                std::cout << "\n";
            }
        }
        std::cout << "[OutfitAudit] randomize " << Wardrobe::GenderName(base.Sex) << ": " << seeds - randomBad << " of " << seeds
                  << " outfits clean";
        for (const auto& [k, n] : problemKinds) std::cout << "; " << k << " x" << n;
        std::cout << "\n";
        for (const auto& [style, n] : styles) {
            std::cout << "    " << style << " " << n << ":";
            for (const auto& slot : w.RandomOrder)
                if (auto it = fills.find(style + "/" + slot); it != fills.end())
                    std::printf(" %s %d%%", slot.c_str(), (int)(100.0f * it->second / n + 0.5f));
            std::cout << "\n";
        }
        bad += randomBad;
    }
    return bad;
}

} // namespace

int Run(AssetLibrary& assets, const std::string& wardrobe, const std::string& csvPath, bool geometry) {
    std::string err;
    auto cat = OutfitSystem::LoadCatalog(assets, wardrobe, true, &err);
    if (!cat) {
        std::cerr << "[OutfitAudit] " << err << "\n";
        return -1;
    }
    const Wardrobe::Wardrobe& w = cat->W;
    const int ruleProblems = CheckRules(*cat, 5000);
    if (!geometry) return ruleProblems;

    std::map<std::string, OutfitCoverage::Mesh> meshes;
    auto mesh = [&](const std::string& path) -> const OutfitCoverage::Mesh& {
        auto it = meshes.find(path);
        if (it != meshes.end()) return it->second;
        OutfitCoverage::Mesh m;
        if (auto model = assets.LoadModel(ProjectPaths::Resolve(path))) model->CollisionGeometry(m.Positions, m.Indices);
        return meshes[path] = std::move(m);
    };

    std::vector<Candidate> all[2];
    std::vector<Pair> pairs;
    for (int g = 0; g < 2; ++g) {
        const Wardrobe::Gender sex = (Wardrobe::Gender)g;
        const auto& body = w.Body(sex);
        if (body.Parts.empty()) continue;
        auto& list = all[g];
        const Wardrobe::RaceDef* race = Wardrobe::FindRace(w, sex, "");
        for (const auto& [part, model] : body.Parts) list.push_back({part, model, true});
        if (race && !race->Head.empty()) list.push_back({"Head", race->Head, true});
        for (const auto& [name, model] : body.Alternates) list.push_back({"Feet", model, true});
        for (const auto& item : cat->Items)
            if (item.Sex == sex) list.push_back({item.Slot, item.Path, false});

        for (const auto& over : list) {
            if (over.BodyPart) continue;
            const auto lo = Wardrobe::LayerOf(w, over.Slot, over.Path, false);
            for (const auto& under : list) {
                if (under.Slot == over.Slot) continue;
                const auto lu = Wardrobe::LayerOf(w, under.Slot, under.Path, under.BodyPart);
                if (!Wardrobe::Hides(lo, under.Slot, lu, over.Slot)) continue;
                // Only pairs the rules let one character wear together, as they are.
                Wardrobe::Request req;
                req.Sex = sex;
                req.Race = race ? race->Name : std::string();
                req.Items[over.Slot] = over.Path;
                if (!under.BodyPart) req.Items[under.Slot] = under.Path;
                const auto resolved = Wardrobe::Resolve(w, cat->Items, req);
                if (!resolved.Dropped.empty() || !resolved.Clashes.empty()) continue;
                auto worn = [&](const Candidate& c) {
                    return std::any_of(resolved.Pieces.begin(), resolved.Pieces.end(),
                                       [&](const Wardrobe::Piece& p) { return p.Slot == c.Slot && p.Path == c.Path; });
                };
                if (worn(over) && worn(under)) pairs.push_back({sex, &under, &over});
            }
        }
    }
    std::cout << "[OutfitAudit] " << pairs.size() << " pairs\n";

    // Geometry on this thread (GL), the maths on every core.
    for (const auto& p : pairs) { mesh(p.Under->Path); mesh(p.Over->Path); }
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::future<void>> jobs;
    for (unsigned t = 0; t < threads; ++t)
        jobs.push_back(std::async(std::launch::async, [&, t] {
            for (size_t i = t; i < pairs.size(); i += threads) {
                Pair& p = pairs[i];
                const auto& u = meshes.at(p.Under->Path);
                const auto& o = meshes.at(p.Over->Path);
                const Wardrobe::SlotDef* us = w.Slot(p.Under->Slot);
                const bool rigid = !p.Under->BodyPart && us && us->HeadAttached; // the game's rule (OutfitSystem::UpdateHiding)
                const auto hidden = OutfitCoverage::Hidden(u, o, p.Under->BodyPart && p.Under->Slot == "Head", rigid);
                const auto poke = OutfitCoverage::PokeDepth(u, o, kMaxDepth);
                p.Verts = (int)u.Positions.size();
                for (size_t v = 0; v < hidden.size(); ++v) {
                    p.Hidden += hidden[v];
                    if (!hidden[v] && poke[v] > kMinDepth) {
                        ++p.Clipping;
                        p.Depth = std::max(p.Depth, poke[v]);
                    }
                }
            }
        }));
    for (auto& j : jobs) j.get();

    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.Clipping > b.Clipping; });
    int clipping = 0;
    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        csv << "gender,under_slot,under,over_slot,over,verts,hidden,clipping,depth_cm\n";
    }
    for (const auto& p : pairs) {
        if (p.Clipping) ++clipping;
        if (csv)
            csv << Wardrobe::GenderName(p.Sex) << ',' << p.Under->Slot << ',' << Wardrobe::Stem(p.Under->Path) << ','
                << p.Over->Slot << ',' << Wardrobe::Stem(p.Over->Path) << ',' << p.Verts << ',' << p.Hidden << ','
                << p.Clipping << ',' << p.Depth * 100.0f << '\n';
    }
    std::cout << "[OutfitAudit] " << clipping << " of " << pairs.size() << " pairs clip\n";
    for (size_t i = 0; i < pairs.size() && i < 40 && pairs[i].Clipping; ++i) {
        const Pair& p = pairs[i];
        std::printf("  %-6s %-28s under %-30s %5d verts poke through (deepest %.1f cm)\n", Wardrobe::GenderName(p.Sex),
                    Wardrobe::Stem(p.Under->Path).c_str(), Wardrobe::Stem(p.Over->Path).c_str(), p.Clipping, p.Depth * 100.0f);
    }
    return clipping + ruleProblems;
}

} // namespace OutfitAudit
