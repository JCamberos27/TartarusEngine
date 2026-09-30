#include "OutfitAudit.h"

#include "AnimationSystem.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Model.h"
#include "Texture.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "Wardrobe.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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
    std::string Race; // a race's own head or part (under it, what's worn is checked on that race)
};

struct Pair {
    Wardrobe::Gender Sex;
    const Candidate* Under;
    const Candidate* Over;
    int Verts = 0, Hidden = 0, Clipping = 0;
    float Depth = 0.0f; // deepest vertex still poking through, metres
    std::vector<std::uint8_t> HiddenBits; // what the game hides (bind pose)
    // Animated: the most under vertices the game draws that end up outside the over piece, in any pose.
    int Posed = 0;
    float PosedDepth = 0.0f;
    std::string PosedWhere;
};

// How far out a poke is looked for: what the game hides (kPokeReach). Past it the look back reaches a surface
// that was never under the cloth (a jaw down through the neck opening to a collar), not a poke - at 15 cm every
// head under a top read as ~1000 vertices poking through.
constexpr float kMaxDepth = OutfitCoverage::kPokeReach;
constexpr float kMinDepth = 0.002f;  // closer than this is the same surface, not a poke
// Animated: how far out a drawn under vertex is looked for past the over piece. Clipping in motion is
// shallow; further than this is, again, a surface that was never under the cloth.
constexpr float kPosedReach = 0.03f;
constexpr float kPosedMinDepth = 0.004f; // a few mm of sway is the cloth's thickness, not a visible poke

// The poses the posed check skins every piece into: what the characters play, and the extremes.
struct PoseClip {
    const char* Name;
    const char* Ref;
};
constexpr PoseClip kPoseClips[] = {
    {"idle", "assets/Animations/Mocap/Idle/AM_Stand_Idle_01.fbx"},
    {"walk", "assets/Animations/Mocap/Locomotion_V2/AM_Walk/AM_Loco_Walk_Fwd.fbx"},
    {"run", "assets/Animations/Mocap/Locomotion_V2/AM_Run/AM_Loco_Run_Fwd.fbx"},
    {"crouch walk", "assets/Animations/Mocap/Locomotion_V2/AM_Crouch_Walk/AM_Crouch_Loco_Walk_Fwd.fbx"},
    {"crouch", "assets/Animations/Mocap/Crouch/AM_Crouch_Idle_01.fbx"},
    {"jump", "assets/Animations/Mocap/Jump/AM_Jump.fbx"},
    {"pickup", "assets/Animations/Mocap/Pickup/AM_Stand_Pickup_02_Floor_To_Floor.fbx"},
    {"arm flare", "assets/Animations/Mocap/Dance/AM_Dance_Basic_03_Arm_Flare.fbx"},
};
constexpr float kPoseTimes[] = {0.1f, 0.35f, 0.6f, 0.85f}; // of each clip's length

// `model`'s geometry (CollisionGeometry order) in `clipRef` at `fraction` of its length. Unskinned (rigid
// headwear): carried by `headFollow`, as OutfitSystem::UpdateAttachments does. False if the clip doesn't resolve.
bool Posed(Model& model, AssetLibrary& assets, const char* clipRef, float fraction, const glm::mat4* headFollow,
           OutfitCoverage::Mesh& out) {
    model.CollisionGeometry(out.Positions, out.Indices);
    if (model.BoneCount() == 0) {
        if (headFollow)
            for (auto& p : out.Positions) p = glm::vec3(*headFollow * glm::vec4(p, 1.0f));
        return true;
    }
    const int clip = ResolveAnimationClip(model, clipRef, assets);
    if (clip < 0) return false;
    std::vector<LocalTRS> pose;
    model.SampleLocalPose(clip, fraction * model.AnimationLength(clip), AnimationWrapMode::ClampForever, pose);
    model.ApplyLocalPose(pose);
    std::vector<glm::mat4> palette((size_t)model.BoneCount());
    for (int i = 0; i < model.BoneCount(); ++i) palette[(size_t)i] = model.FinalBoneMatrix(i);
    size_t k = 0;
    for (int mi = 0; mi < model.MeshCount(); ++mi)
        for (const auto& v : model.MeshSkinVertices(mi)) {
            glm::mat4 m(0.0f);
            float tw = 0.0f;
            for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                if (v.BoneIDs[i] >= 0 && v.BoneIDs[i] < (int)palette.size()) {
                    m += palette[(size_t)v.BoneIDs[i]] * v.Weights[i];
                    tw += v.Weights[i];
                }
            if (k < out.Positions.size() && tw > 1e-4f) out.Positions[k] = glm::vec3(m * glm::vec4(out.Positions[k], 1.0f));
            ++k;
        }
    return k == out.Positions.size();
}

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

int Run(AssetLibrary& assets, const std::string& wardrobe, const std::string& csvPath, bool geometry, bool posed) {
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
        if (auto model = assets.LoadModel(ProjectPaths::Resolve(path))) {
            model->CollisionGeometry(m.Positions, m.Indices);
            if (model->BoneCount() == 0) { // rigid head wear, at its fit (as OutfitSystem places it)
                const glm::mat4 fit = OutfitSystem::FitMatrix(*model, Wardrobe::FitOf(w, path));
                for (auto& q : m.Positions) q = glm::vec3(fit * glm::vec4(q, 1.0f));
            }
        }
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
        // Every race's head and its own parts: the heads differ in shape (a bun, a fuller skull), and a
        // balaclava fitted to one pokes through on another.
        for (const auto& r : body.Races) {
            if (!r.Head.empty()) list.push_back({"Head", r.Head, true, r.Name});
            for (const auto& [part, model] : r.Parts) list.push_back({part, model, true, r.Name});
        }
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
                req.Race = !under.Race.empty() ? under.Race : race ? race->Name : std::string();
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
                p.HiddenBits = hidden;
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

    // Animated: every piece skinned into each pose (here: models are GL-loaded and hold pose state), then per
    // pair the under vertices the game draws that end up outside the over piece (on every core).
    if (posed) {
        std::map<std::string, int> sexOf;
        for (const auto& p : pairs) { sexOf[p.Under->Path] = (int)p.Sex; sexOf[p.Over->Path] = (int)p.Sex; }
        std::string heads[2];
        for (int g = 0; g < 2; ++g)
            if (const Wardrobe::RaceDef* race = Wardrobe::FindRace(w, (Wardrobe::Gender)g, "")) heads[g] = race->Head;
        const auto t0 = std::chrono::steady_clock::now();
        int poses = 0;
        // Poke depths over every pair and pose: <=4 mm (kPosedMinDepth - what the draw's layer pull, OutfitLayerTag,
        // keeps behind), 4-5, 5-10, 10-15, 15-20, 20-25, 25-30 mm.
        std::atomic<long long> depthHist[7] = {};
        for (const PoseClip& clip : kPoseClips)
            for (float at : kPoseTimes) {
                char where[64];
                std::snprintf(where, sizeof where, "%s @%d%%", clip.Name, (int)(at * 100.0f + 0.5f));
                glm::mat4 follow[2];
                bool haveFollow[2] = {false, false};
                for (int g = 0; g < 2; ++g) {
                    auto head = heads[g].empty() ? nullptr : assets.LoadModel(ProjectPaths::Resolve(heads[g]));
                    const int bone = head ? head->BoneId("head") : -1;
                    OutfitCoverage::Mesh scratch;
                    if (bone >= 0 && Posed(*head, assets, clip.Ref, at, nullptr, scratch)) {
                        follow[g] = head->FinalBoneMatrix(bone);
                        haveFollow[g] = true;
                    }
                }
                std::map<std::string, OutfitCoverage::Mesh> posedMeshes;
                bool ok = true;
                for (const auto& [path, sex] : sexOf) {
                    auto model = assets.LoadModel(ProjectPaths::Resolve(path));
                    if (!model) continue;
                    OutfitCoverage::Mesh m;
                    const glm::mat4 fitted = haveFollow[sex] ? follow[sex] * OutfitSystem::FitMatrix(*model, Wardrobe::FitOf(w, path)) : glm::mat4(1.0f);
                    if (!Posed(*model, assets, clip.Ref, at, haveFollow[sex] ? &fitted : nullptr, m)) {
                        ok = false;
                        break;
                    }
                    posedMeshes[path] = std::move(m);
                }
                if (!ok) {
                    std::cerr << "[OutfitAudit] clip " << clip.Ref << " doesn't resolve - skipped\n";
                    break;
                }
                ++poses;
                std::vector<std::future<void>> posedJobs;
                for (unsigned t = 0; t < threads; ++t)
                    posedJobs.push_back(std::async(std::launch::async, [&, t] {
                        for (size_t i = t; i < pairs.size(); i += threads) {
                            Pair& p = pairs[i];
                            const auto u = posedMeshes.find(p.Under->Path), o = posedMeshes.find(p.Over->Path);
                            if (u == posedMeshes.end() || o == posedMeshes.end()) continue;
                            const auto poke = OutfitCoverage::PokeDepth(u->second, o->second, kPosedReach);
                            int n = 0;
                            float deepest = 0.0f;
                            for (size_t v = 0; v < poke.size() && v < p.HiddenBits.size(); ++v)
                                if (!p.HiddenBits[v] && poke[v] > 0.0f && poke[v] <= kPosedMinDepth) ++depthHist[0];
                                else if (!p.HiddenBits[v] && poke[v] > kPosedMinDepth) {
                                    ++depthHist[std::min(6, 1 + (int)(poke[v] / 0.005f))];
                                    ++n;
                                    deepest = std::max(deepest, poke[v]);
                                }
                            if (n > p.Posed) { p.Posed = n; p.PosedDepth = deepest; p.PosedWhere = where; }
                        }
                    }));
                for (auto& j : posedJobs) j.get();
            }
        std::printf("[OutfitAudit] posed poke depths (vertex-poses): <=4mm %lld | 4-5 %lld | 5-10 %lld | 10-15 %lld | 15-20 %lld | 20-25 %lld | 25-30 %lld\n",
                    depthHist[0].load(), depthHist[1].load(), depthHist[2].load(), depthHist[3].load(), depthHist[4].load(),
                    depthHist[5].load(), depthHist[6].load());
        std::printf("[OutfitAudit] posed check: %d poses in %.1f s\n", poses,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        return a.Posed != b.Posed ? a.Posed > b.Posed : a.Clipping > b.Clipping;
    });
    int clipping = 0;
    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        csv << "gender,under_slot,under,over_slot,over,verts,hidden,clipping,depth_cm,posed,posed_depth_cm,posed_where\n";
    }
    int posedClipping = 0;
    for (const auto& p : pairs) {
        if (p.Clipping) ++clipping;
        if (p.Posed) ++posedClipping;
        if (csv)
            csv << Wardrobe::GenderName(p.Sex) << ',' << p.Under->Slot << ',' << Wardrobe::Stem(p.Under->Path) << ','
                << p.Over->Slot << ',' << Wardrobe::Stem(p.Over->Path) << ',' << p.Verts << ',' << p.Hidden << ','
                << p.Clipping << ',' << p.Depth * 100.0f << ',' << p.Posed << ',' << p.PosedDepth * 100.0f << ','
                << p.PosedWhere << '\n';
    }
    std::cout << "[OutfitAudit] bind pose: " << clipping << " of " << pairs.size() << " pairs clip\n";
    if (posed) std::cout << "[OutfitAudit] animated: " << posedClipping << " of " << pairs.size() << " pairs clip in some pose\n";
    for (size_t i = 0; i < pairs.size() && i < 40 && (pairs[i].Clipping || pairs[i].Posed); ++i) {
        const Pair& p = pairs[i];
        std::printf("  %-6s %-28s under %-30s bind %4d  posed %5d verts (deepest %.1f cm, %s)\n", Wardrobe::GenderName(p.Sex),
                    Wardrobe::Stem(p.Under->Path).c_str(), Wardrobe::Stem(p.Over->Path).c_str(), p.Clipping, p.Posed,
                    p.PosedDepth * 100.0f, p.PosedWhere.c_str());
    }
    // Animated clipping is reported, not failed on: cloth sways off the skin it was hidden against by design
    // (the edge band); the count is for comparing before and after a change.
    return clipping + ruleProblems;
}

namespace {
std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
} // namespace

int RunSelfTest(AssetLibrary& assets, const std::string& wardrobe) {
    std::string err;
    const auto cat = OutfitSystem::LoadCatalog(assets, wardrobe, false, &err);
    if (!cat) {
        std::cerr << "[OutfitSelfTest] " << err << "\n";
        return -1;
    }
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok) { ++failures; std::cout << "[OutfitSelfTest] FAIL " << what << "\n"; }
    };
    auto item = [&](const std::string& rel) {
        const auto* it = cat->Find("assets/Characters/Quantum/Models/Clothing/" + rel);
        check(it != nullptr, "wardrobe has " + rel);
        return it ? it->Path : std::string();
    };
    auto stemIn = [](const World& w, entt::entity root, const std::string& slot) {
        const auto pieces = OutfitSystem::Pieces(w, root);
        auto it = pieces.find(slot);
        return it == pieces.end() ? std::string() : Wardrobe::Stem(w.Registry.get<OutfitPieceComponent>(it->second).Item);
    };
    auto character = [&](World& w) {
        const entt::entity e = w.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Guy");
        w.Registry.emplace<CharacterOutfitComponent>(e).Wardrobe = wardrobe;
        return e;
    };
    // Lets every background load and pending change land (what the frame loop does).
    auto settle = [&](World& w) {
        for (int i = 0; i < 2000; ++i) {
            assets.PumpAsync(50.0);
            OutfitSystem::UpdatePending(w, assets);
            bool any = false;
            for (auto e : w.Registry.view<CharacterOutfitComponent>()) any |= OutfitSystem::IsPending(w, e);
            if (!any) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    Wardrobe::Request base;
    base.Race = "European";
    base.Items = {{"Top", item("Male/Tops/SKM_Tshirt.fbx")}, {"Pants", item("Male/Pants/SKM_Jeans.fbx")},
                  {"Shoes", item("Male/Shoes/SKM_Sneakers.fbx")}, {"Balaclava", item("Male/Balaclava/SM_Balaclava_Crime.fbx")}};

    // Apply: the body, the items, the component's gender and race, a new version.
    {
        World w;
        const entt::entity guy = character(w);
        const auto r = OutfitSystem::Apply(w, assets, guy, base);
        check(r.Ok && r.Created >= 8, "Apply builds the body and items");
        check(stemIn(w, guy, "Top") == "SKM_Tshirt" && stemIn(w, guy, "Head") == "Quantum_Head", "Apply: top and head");
        const auto& outfit = w.Registry.get<CharacterOutfitComponent>(guy);
        check(outfit.Gender == 0 && outfit.Race == "European" && outfit.Version > 0, "Apply sets gender, race and version");
        const auto cur = OutfitSystem::CurrentRequest(w, guy);
        check(cur.Items == base.Items && cur.Race == "European", "CurrentRequest round-trips what Apply built");

        // Equip / take off, through Submit.
        OutfitSystem::Equip(w, assets, guy, "Top", item("Male/Tops/SKM_Hoodie.fbx"));
        settle(w);
        check(stemIn(w, guy, "Top") == "SKM_Hoodie", "Equip swaps the top");
        OutfitSystem::Equip(w, assets, guy, "Top", "");
        settle(w);
        check(stemIn(w, guy, "Top").empty(), "Equip \"\" takes the top off");
        // A pair rule: boots put the pants' _Inboots cut on.
        OutfitSystem::Equip(w, assets, guy, "Shoes", item("Male/Shoes/SKM_Boots.fbx"));
        settle(w);
        check(stemIn(w, guy, "Pants") == "SKM_Jeans_Inboots", "boots take the pants' _Inboots cut");

        // Race and gender.
        OutfitSystem::SetRace(w, assets, guy, "Afro");
        settle(w);
        check(stemIn(w, guy, "Head") == "Quantum_Head_Afro" && w.Registry.get<CharacterOutfitComponent>(guy).Race == "Afro",
              "SetRace swaps the head");
        OutfitSystem::SetGender(w, assets, guy, Wardrobe::Gender::Female);
        settle(w);
        check(w.Registry.get<CharacterOutfitComponent>(guy).Gender == 1 && stemIn(w, guy, "Head").rfind("SKM_F_", 0) == 0 &&
                  stemIn(w, guy, "Balaclava") == "SM_F_Balaclava_Crime",
              "SetGender swaps body and items for the female cuts (gender " +
                  std::to_string(w.Registry.get<CharacterOutfitComponent>(guy).Gender) + ", head " + stemIn(w, guy, "Head") +
                  ", balaclava " + stemIn(w, guy, "Balaclava") + ", pants " + stemIn(w, guy, "Pants") + ")");
    }

    // Randomize: locked slots stay, the same seed gives the same outfit.
    {
        World w1, w2;
        const entt::entity a = character(w1), b = character(w2);
        OutfitSystem::Apply(w1, assets, a, base);
        OutfitSystem::Apply(w2, assets, b, base);
        w1.Registry.get<CharacterOutfitComponent>(a).Locks = "Pants";
        w2.Registry.get<CharacterOutfitComponent>(b).Locks = "Pants";
        for (unsigned seed : {7u, 11u, 12345u}) {
            OutfitSystem::Randomize(w1, assets, a, seed);
            OutfitSystem::Randomize(w2, assets, b, seed);
            settle(w1);
            settle(w2);
            check(stemIn(w1, a, "Pants").rfind("SKM_Jeans", 0) == 0, "Randomize keeps a locked slot");
            check(OutfitSystem::CurrentRequest(w1, a).Items == OutfitSystem::CurrentRequest(w2, b).Items,
                  "Randomize: same seed, same outfit");
        }
    }

    // Colourways, presets.
    {
        World w;
        const entt::entity guy = character(w);
        Wardrobe::Request req = base;
        OutfitSystem::Apply(w, assets, guy, req);
        entt::entity top = OutfitSystem::Pieces(w, guy)["Pants"];
        auto groups = OutfitSystem::ColourGroups(w, assets, top);
        check(!groups.empty() && groups[0].Options.size() >= 2, "the jeans have colourways");
        std::string picked;
        if (!groups.empty() && groups[0].Options.size() >= 2) {
            const auto& g = groups[0];
            picked = g.Options[0] == g.Current ? g.Options[1] : g.Options[0];
            const auto before = w.Registry.get<CharacterOutfitComponent>(guy).Version;
            check(OutfitSystem::SetColourway(w, assets, top, g.Source, picked), "SetColourway");
            check(OutfitSystem::ColourGroups(w, assets, top)[0].Current == picked, "ColourGroups reports the new colourway");
            // Other views of the character (the first-person body's twins) only notice a change by the version.
            check(w.Registry.get<CharacterOutfitComponent>(guy).Version != before, "SetColourway bumps the outfit's version");
        }
        const std::string presetRel = "Library/OutfitSelfTest.outfit";
        std::string saveErr;
        check(OutfitSystem::SavePreset(w, assets, guy, presetRel, &saveErr), "SavePreset: " + saveErr);
        World w2;
        const entt::entity other = character(w2);
        const auto lr = OutfitSystem::LoadPreset(w2, assets, other, presetRel);
        settle(w2);
        check(lr.Ok && OutfitSystem::CurrentRequest(w2, other).Items == OutfitSystem::CurrentRequest(w, guy).Items,
              "LoadPreset puts the same items on");
        if (!picked.empty()) {
            const auto g2 = OutfitSystem::ColourGroups(w2, assets, OutfitSystem::Pieces(w2, other)["Pants"]);
            check(!g2.empty() && g2[0].Current == picked, "LoadPreset puts the colourway back");
        }
        // The same colourway survives boots switching the jeans to their _Inboots cut.
        if (!picked.empty()) {
            OutfitSystem::Equip(w, assets, guy, "Shoes", item("Male/Shoes/SKM_Boots.fbx"));
            settle(w);
            const auto g3 = OutfitSystem::ColourGroups(w, assets, OutfitSystem::Pieces(w, guy)["Pants"]);
            const std::string now = g3.empty() ? std::string() : Lower(Wardrobe::Stem(g3[0].Current));
            check(now == Lower(Wardrobe::Stem(picked)) || now == Lower(Wardrobe::Stem(picked)) + "_inboots",
                  "a colourway follows the pants to their _Inboots cut (" + (g3.empty() ? std::string("none") : g3[0].Current) + ")");
        }
        // A malformed preset is an error, not a crash.
        {
            std::ofstream(ProjectPaths::Resolve(presetRel)) << R"({"gender": 3, "items": {"Top": {"item": 5}}})";
            bool threw = false;
            OutfitSystem::Result bad;
            try { bad = OutfitSystem::LoadPreset(w2, assets, other, presetRel); } catch (...) { threw = true; }
            check(!threw && !bad.Ok, "LoadPreset on a malformed file returns an error");
        }
        std::error_code ec;
        std::filesystem::remove(ProjectPaths::Resolve(presetRel), ec);
    }

    // Submit: a colourway picked, then an item equipped before it landed - both happen.
    {
        World w;
        const entt::entity guy = character(w);
        OutfitSystem::Apply(w, assets, guy, base);
        const entt::entity pants = OutfitSystem::Pieces(w, guy)["Pants"];
        const auto groups = OutfitSystem::ColourGroups(w, assets, pants);
        if (!groups.empty() && groups[0].Options.size() >= 2) {
            const auto& g = groups[0];
            // Something not loaded yet, so it has to wait.
            std::string picked;
            for (const auto& o : g.Options)
                if (o != g.Current) picked = o;
            assets.ForgetRemoved(ProjectPaths::Resolve(picked));
            OutfitSystem::SubmitColourway(w, assets, guy, pants, g.Source, picked);
            OutfitSystem::Equip(w, assets, guy, "Top", item("Male/Tops/SKM_Hoodie.fbx"));
            settle(w);
            const auto after = OutfitSystem::ColourGroups(w, assets, OutfitSystem::Pieces(w, guy)["Pants"]);
            check(stemIn(w, guy, "Top") == "SKM_Hoodie" && !after.empty() && after[0].Current == picked,
                  "a waiting colourway survives an Equip right after it");
        }
    }

    // Duplicate slots: a second piece in a slot (a duplicated child) is removed by the next Apply.
    {
        World w;
        const entt::entity guy = character(w);
        OutfitSystem::Apply(w, assets, guy, base);
        const auto model = assets.InstantiateModel(ProjectPaths::Resolve(base.Items["Top"]));
        const entt::entity dup = w.CreateModelEntity(model, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Top");
        w.AttachChildRaw(dup, guy);
        w.Registry.emplace<OutfitPieceComponent>(dup, OutfitPieceComponent{"Top", base.Items["Top"], 0});
        OutfitSystem::Equip(w, assets, guy, "Top", item("Male/Tops/SKM_Hoodie.fbx"));
        settle(w);
        int tops = 0;
        for (auto [e, p] : w.Registry.view<OutfitPieceComponent>().each()) tops += p.Slot == "Top";
        check(tops == 1, "a duplicated piece in a slot doesn't stay behind (" + std::to_string(tops) + " tops)");
    }

    // Follower animators: pieces follow the body's Animator Controller.
    {
        World w;
        const entt::entity guy = character(w);
        auto& ac = w.Registry.emplace<AnimatorControllerComponent>(guy);
        ac.Controller = "assets/Characters/Quantum/Quantum.controller"; // any path: only the copy is checked
        OutfitSystem::Apply(w, assets, guy, base);
        int followers = 0, pieces = 0;
        for (auto [e, p] : w.Registry.view<OutfitPieceComponent>().each()) {
            ++pieces;
            if (const auto* f = w.Registry.try_get<AnimatorControllerComponent>(e); f && f->Driver == guy) ++followers;
        }
        check(pieces > 0 && followers == pieces, "every piece follows the body's controller (" + std::to_string(followers) + "/" +
                                                     std::to_string(pieces) + ")");
    }

    // Hiding: the tags go on, and a piece's slot or item edited in place is noticed.
    {
        World w;
        const entt::entity guy = character(w);
        OutfitSystem::Apply(w, assets, guy, base);
        for (int i = 0; i < 3000 && !w.Registry.get<CharacterOutfitComponent>(guy).HideSignature; ++i) {
            w.RebuildWorldTransformCache();
            OutfitSystem::UpdateHiding(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto sig = w.Registry.get<CharacterOutfitComponent>(guy).HideSignature;
        check(sig != 0 && w.Registry.all_of<OutfitHideTag>(OutfitSystem::Pieces(w, guy)["Torso"]), "the torso under the shirt is hidden");
        w.Registry.get<OutfitPieceComponent>(OutfitSystem::Pieces(w, guy)["Top"]).Item = item("Male/Tops/SKM_Tshirt_Tucked.fbx");
        // New coverage may take a few frames to work out; until then the old signature stays.
        for (int i = 0; i < 3000 && w.Registry.get<CharacterOutfitComponent>(guy).HideSignature == sig; ++i) {
            OutfitSystem::UpdateHiding(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(w.Registry.get<CharacterOutfitComponent>(guy).HideSignature != sig, "editing a piece's item re-runs the hiding");
    }

    // AdoptExisting: untagged children are recognised as the outfit's pieces.
    {
        World w;
        const entt::entity guy = character(w);
        OutfitSystem::Apply(w, assets, guy, base);
        const auto before = OutfitSystem::Pieces(w, guy).size();
        for (auto e : std::vector<entt::entity>(w.Registry.view<OutfitPieceComponent>().begin(), w.Registry.view<OutfitPieceComponent>().end()))
            w.Registry.remove<OutfitPieceComponent>(e);
        const int adopted = OutfitSystem::AdoptExisting(w, assets, guy);
        check(adopted == (int)before && OutfitSystem::Pieces(w, guy).size() == before,
              "AdoptExisting finds every piece (" + std::to_string(adopted) + "/" + std::to_string(before) + ")");
        check(stemIn(w, guy, "Feet") == "Quantum_Feet_Shoes" || stemIn(w, guy, "Feet") == "Quantum_Feet", "AdoptExisting: feet");
    }

    // CancelPending drops a waiting change.
    {
        World w;
        const entt::entity guy = character(w);
        OutfitSystem::Apply(w, assets, guy, base);
        assets.ForgetRemoved(ProjectPaths::Resolve(item("Male/Tops/SKM_Jersey.fbx")));
        const auto r = OutfitSystem::Equip(w, assets, guy, "Top", item("Male/Tops/SKM_Jersey.fbx"));
        OutfitSystem::CancelPending(w);
        check(!OutfitSystem::IsPending(w, guy), "CancelPending drops the waiting change");
        settle(w);
        check(!r.Pending || stemIn(w, guy, "Top") == "SKM_Tshirt", "a cancelled change isn't applied");
    }

    std::cout << "[OutfitSelfTest] " << checks << " checks, " << failures << " failure(s)\n";
    return failures;
}

int RunCost(AssetLibrary& assets, const std::string& wardrobe, const std::vector<std::string>& scenes) {
    std::string err;
    const auto cat = OutfitSystem::LoadCatalog(assets, wardrobe, false, &err);
    if (!cat) {
        std::cerr << "[OutfitCost] " << err << "\n";
        return -1;
    }
    const Wardrobe::Wardrobe& w = cat->W;

    // 1. Every model the wardrobe can put on a character: what one copy of it costs to draw.
    struct Row { std::string Slot, Path; unsigned Tris = 0, Verts = 0; int Meshes = 0, Bones = 0; };
    std::vector<Row> rows;
    std::set<std::string> seen;
    auto add = [&](const std::string& slot, const std::string& path) {
        if (!seen.insert(path).second) return;
        Row r{slot, path};
        if (auto model = assets.LoadModel(ProjectPaths::Resolve(path))) {
            r.Meshes = model->MeshCount();
            for (int i = 0; i < r.Meshes; ++i) { r.Tris += model->MeshTriangleCount(i); r.Verts += model->MeshVertexCount(i); }
            r.Bones = model->BoneCount();
        } else {
            std::cout << "[OutfitCost] can't load " << path << "\n";
        }
        rows.push_back(r);
    };
    for (int g = 0; g < 2; ++g) {
        const auto& body = w.Body((Wardrobe::Gender)g);
        for (const auto& [part, model] : body.Parts) add(part, model);
        for (const auto& race : body.Races) {
            if (!race.Head.empty()) add("Head", race.Head);
            for (const auto& [part, model] : race.Parts) add(part, model);
        }
        for (const auto& [name, model] : body.Alternates) add(name, model);
    }
    for (const auto& item : cat->Items) add(item.Slot, item.Path);

    // The model files' own materials' textures: loaded with the model (uncompressed, the default import size)
    // even where a material remap replaces them on every piece.
    {
        std::set<const Texture*> embedded;
        unsigned long long bytes = 0;
        int models = 0;
        for (const auto& r : rows) {
            auto model = assets.LoadModel(ProjectPaths::Resolve(r.Path));
            if (!model) continue;
            bool any = false;
            for (int i = 0; i < model->MeshCount(); ++i) {
                const Material& m = model->MeshMaterial(i);
                for (const Texture* t : {m.AlbedoMap.get(), m.NormalMap.get(), m.MetallicRoughnessMap.get(), m.MetallicMap.get(),
                                         m.RoughnessMap.get(), m.AOMap.get(), m.EmissiveMap.get()})
                    if (t && embedded.insert(t).second) {
                        bytes += 4ull * t->Width() * t->Height() * 4 / 3;
                        any = true;
                    }
            }
            models += any;
        }
        std::printf("[OutfitCost] the model files' own textures: %zu loaded by %d models, ~%.0f MB of VRAM\n", embedded.size(),
                    models, bytes / 1048576.0);
    }

    std::map<std::string, std::pair<unsigned long long, int>> bySlot; // slot -> (tris, models)
    for (const auto& r : rows) { bySlot[r.Slot].first += r.Tris; ++bySlot[r.Slot].second; }
    std::cout << "[OutfitCost] " << rows.size() << " models; average triangles per model by slot:\n";
    for (const auto& [slot, v] : bySlot)
        std::printf("    %-12s %3d models  %7llu tris avg\n", slot.c_str(), v.second, v.first / std::max(1, v.second));
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.Tris > b.Tris; });
    std::cout << "[OutfitCost] heaviest models:\n";
    for (size_t i = 0; i < rows.size() && i < 25; ++i)
        std::printf("    %7u tris %7u verts %2d meshes %3d bones  %-11s %s\n", rows[i].Tris, rows[i].Verts, rows[i].Meshes,
                    rows[i].Bones, rows[i].Slot.c_str(), Wardrobe::Stem(rows[i].Path).c_str());

    // 2. Each scene as it's drawn: pieces, triangles, and how much of it hiding throws away (vertices
    // hidden under clothes still go through the vertex shader and, per piece, the shadow passes).
    for (const auto& scene : scenes) {
        World world;
        if (!SceneSerializer::Load(world, assets, ProjectPaths::Resolve(scene), false)) {
            std::cout << "[OutfitCost] can't load " << scene << "\n";
            continue;
        }
        // Coverage comes from Library/OutfitCoverage or is worked out in the background; wait for it.
        for (int frame = 0; frame < 6000; ++frame) {
            world.RebuildWorldTransformCache();
            assets.PumpAsync(50.0);
            OutfitSystem::UpdatePending(world, assets);
            OutfitSystem::UpdateHiding(world);
            bool waiting = false;
            for (auto [e, outfit] : world.Registry.view<CharacterOutfitComponent>().each())
                if (outfit.AutoHide && outfit.HideSignature == 0) waiting = true;
            if (!waiting && frame > 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        int characters = 0, pieces = 0, fullyHidden = 0, drawCalls = 0, hiddenMeshes = 0;
        unsigned long long tris = 0, verts = 0, hidden = 0, hiddenPieceTris = 0, hiddenTris = 0;
        std::map<std::string, unsigned long long> slotTris, slotHiddenTris;
        // Each model's triangles (model-wide vertex numbers, as the hide bits count them) and where each
        // sub-mesh's triangles start.
        struct Tris { std::vector<unsigned> Indices; std::vector<size_t> MeshStart; };
        std::map<const Model*, Tris> modelTris;
        characters = (int)world.Registry.view<CharacterOutfitComponent>().size();
        for (auto [e, piece, rc] : world.Registry.view<OutfitPieceComponent, RenderableComponent>().each()) {
            if (!rc.ModelRef) continue;
            ++pieces;
            unsigned t = 0;
            for (int i = 0; i < rc.ModelRef->MeshCount(); ++i) t += rc.ModelRef->MeshTriangleCount(i);
            drawCalls += rc.ModelRef->MeshCount();
            tris += t;
            slotTris[piece.Slot] += t;
            const auto* tag = world.Registry.try_get<OutfitHideTag>(e);
            verts += tag ? tag->Total : rc.ModelRef->VertexCount();
            if (tag) {
                hidden += tag->Hidden;
                if (tag->Total && tag->Hidden >= tag->Total) { ++fullyHidden; hiddenPieceTris += t; }
            }
            if (tag && tag->Bits) {
                auto& mt = modelTris[rc.ModelRef.get()];
                if (mt.Indices.empty()) {
                    std::vector<glm::vec3> positions;
                    rc.ModelRef->CollisionGeometry(positions, mt.Indices);
                    size_t start = 0;
                    for (int i = 0; i < rc.ModelRef->MeshCount(); ++i) {
                        mt.MeshStart.push_back(start);
                        start += 3ull * rc.ModelRef->MeshTriangleCount(i);
                    }
                    mt.MeshStart.push_back(start);
                }
                const auto& bits = *tag->Bits;
                auto isHidden = [&](unsigned v) { return (v >> 5) < bits.size() && ((bits[v >> 5] >> (v & 31)) & 1u); };
                for (size_t m = 0; m + 1 < mt.MeshStart.size(); ++m) {
                    unsigned meshHidden = 0, meshTris = 0;
                    for (size_t i = mt.MeshStart[m]; i + 2 < mt.MeshStart[m + 1] && i + 2 < mt.Indices.size(); i += 3) {
                        ++meshTris;
                        if (isHidden(mt.Indices[i]) && isHidden(mt.Indices[i + 1]) && isHidden(mt.Indices[i + 2])) ++meshHidden;
                    }
                    hiddenTris += meshHidden;
                    slotHiddenTris[piece.Slot] += meshHidden;
                    // One character's sub-meshes of each body model, to see where a heavy piece's triangles are.
                    static std::set<std::string> shown;
                    if ((piece.Flags & OutfitPieceBodyPart) && shown.insert(scene + rc.ModelRef->Path() + std::to_string(m)).second)
                        std::printf("      sub-mesh %-40s %6u tris %3.0f%% hidden  (%s)\n", rc.ModelRef->MeshMaterial((int)m).Name.c_str(),
                                    meshTris, meshTris ? 100.0 * meshHidden / meshTris : 0.0, Wardrobe::Stem(rc.ModelRef->Path()).c_str());
                    if (meshTris && meshHidden == meshTris) ++hiddenMeshes;
                }
            }
        }
        std::printf("[OutfitCost] %s: %d characters, %d pieces, %d draws, %.2fM tris (%.0fk per character), "
                    "%.0f%% of vertices hidden, %d pieces fully hidden (%.2fM tris); %.2fM tris (%.0f%%) have every "
                    "vertex hidden, %d sub-mesh draws entirely\n",
                    scene.c_str(), characters, pieces, drawCalls, tris / 1e6, characters ? tris / 1e3 / characters : 0.0,
                    verts ? 100.0 * hidden / verts : 0.0, fullyHidden, hiddenPieceTris / 1e6, hiddenTris / 1e6,
                    tris ? 100.0 * hiddenTris / tris : 0.0, hiddenMeshes);
        std::vector<std::pair<unsigned long long, std::string>> order;
        for (const auto& [slot, t] : slotTris) order.push_back({t, slot});
        std::sort(order.rbegin(), order.rend());
        for (const auto& [t, slot] : order)
            std::printf("    %-12s %6.2fM tris, %3.0f%% hidden\n", slot.c_str(), t / 1e6, t ? 100.0 * slotHiddenTris[slot] / t : 0.0);
    }
    return 0;
}

} // namespace OutfitAudit
