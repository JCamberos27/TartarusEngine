#include "AudioTest.h"

#include "AssetLibrary.h"
#include "AudioEngine.h"
#include "Audio/ReverbZones.h"
#include "Audio/WeaponAudio.h"
#include "Components.h"
#include "LoudnessMeter.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "Window.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The audio engine's own test (docs/AUDIO.md, "--audio-test"). Offline: the real engine (voices, buses, the convolution reverb, the
// master limiter) rendered on demand at 48 kHz with no device, driven by the real WeaponAudio over a scene's Reverb Zones. Four parts:
//  0. The tier ladder: the spec's levels and distance models, checked against the hierarchy's rules (nothing outranks the player's
//     gun up close, soldiers' steps carry over the beds, ...). No audio.
//  1. Levels: every manifest key played alone, dry (no reverb, no jitter, no ducking), LUFS-M max against tools/audio/recipes/mix.json
//     (reference + the key's level + its surface / space offset). 3D at its distance model's reference distance, and close up (the cap).
//  2. Wet / dry: in each zone, an impact 2 m in front; the reverb return's energy against the dry signal's, against the zone's
//     calibrated level (Reverb Bus: Wet <class> dB + the zone's trim, x the impacts' send).
//  3. Ambience: in each zone with a bed, the bed's LUFS-M max against the spec's ambience level for its class.
//  4. Peak: a full-auto burst with impacts and a soldier's shots in the loudest zone; the master stays under the limiter's ceiling.
//  5. Firefight: 20 s of a scripted fight with the whole dynamic mix on (ducking, air, glue): integrated loudness, how hard the glue
//     and the limiter work, how deep the beds duck.
// Exit 1 when a level is off by more than 2 dB, a wet / dry by more than 3 dB, the peak is over the ceiling, a rule of the ladder or
// the firefight's bounds fails, or a part could not run.

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr int kRate = 48000;
constexpr int kBlock = 480; // 10 ms, the game's step and a typical device callback
constexpr float kLevelTolDb = 2.0f;
constexpr float kWetTolDb = 3.0f;

std::ofstream s_Report;
int s_Checks = 0, s_Failures = 0;

void Out(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout); // (a crash must not take the lines before it)
    if (s_Report.is_open()) s_Report << buf;
}

bool Check(bool ok) {
    ++s_Checks;
    if (!ok) ++s_Failures;
    return ok;
}

bool ReadJson(const fs::path& p, json& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = json::parse(ss.str(), nullptr, false);
    return !out.is_discarded() && out.is_object();
}

bool Num(const json& j, const char* key, float& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<float>();
    return true;
}

// The spec's level of a key (dB re the player's shot) and the distance it holds at (0 = 2D). False for keys the spec does not level
// here (the shot layers are measured as whole shots; impulse responses are not sounds).
bool SpecLevel(const json& spec, const std::string& key, float& level, std::string& refName) {
    const json& L = spec["levels"];
    const json& surf = spec.contains("surface_db") ? spec["surface_db"] : json::object();
    auto surface = [&](const std::string& s) {
        float d = 0.0f;
        Num(surf, s.c_str(), d);
        return d;
    };
    refName.clear();
    auto parts = [&](size_t from) {
        std::vector<std::string> p;
        std::string cur;
        for (size_t i = from; i <= key.size(); ++i) {
            if (i == key.size() || key[i] == '.') {
                p.push_back(cur);
                cur.clear();
            } else {
                cur += key[i];
            }
        }
        return p;
    };
    if (key.rfind("snd.", 0) != 0) return false;
    const std::vector<std::string> p = parts(4);
    if (p.empty()) return false;
    if (p[0] == "foley" && p.size() == 3) {
        if (p[1] == "weapon") return L.contains("gear") && Num(L["gear"], p[2].c_str(), level);
        if (p[1] == "move") return L.contains("move") && Num(L["move"], p[2].c_str(), level);
        if (p[1].rfind("step_", 0) == 0 && L.contains("step") && Num(L["step"], p[2].c_str(), level)) {
            level += surface(p[1].substr(5));
            return true;
        }
        return false;
    }
    if (p[0] == "casing" && p.size() == 3 && Num(L, "casing", level)) {
        level += surface(p[2]);
        return true;
    }
    if (p[0] == "impact" && p.size() == 2) {
        if (p[1] == "flesh") return Num(L, "flesh", level);
        if (!Num(L, "impact", level)) return false;
        level += surface(p[1]);
        refName = "impact";
        return true;
    }
    if (p[0] == "flyby" && p.size() == 1) {
        refName = "flyby";
        return Num(L, "flyby", level);
    }
    if (p[0] == "body_fall" && p.size() == 1) {
        refName = "body_fall";
        return Num(L, "body_fall", level);
    }
    if (p[0] == "ui" && p.size() == 2) return L.contains("ui") && Num(L["ui"], p[1].c_str(), level);
    if (p[0] == "amb" && p.size() == 2) return L.contains("ambience") && Num(L["ambience"], p[1].c_str(), level);
    if (p.size() == 2) { // snd.<gun>.<element>
        if (p[1].rfind("fire_", 0) == 0) return false;
        if (Num(L, p[1].c_str(), level)) return true; // melee_hit, melee_swing
        return L.contains("elements") && Num(L["elements"], p[1].c_str(), level);
    }
    return false;
}

// A distance model of the manifest's mix.distance (empty: none).
DistanceModel ModelOf(const json& manifest, const std::string& name) {
    DistanceModel m;
    if (name.empty() || !manifest.contains("mix") || !manifest["mix"].contains("distance")) return m;
    const json& d = manifest["mix"]["distance"];
    if (!d.contains(name) || !d[name].is_object()) return m;
    Num(d[name], "ref_m", m.RefM);
    Num(d[name], "slope_db", m.SlopeDb);
    Num(d[name], "near_db", m.NearDb);
    Num(d[name], "max_m", m.MaxM);
    Num(d[name], "offset_db", m.OffsetDb);
    return m;
}

// The offline engine, stepped like the game: WeaponAudio's update, then one 10 ms block rendered, the taps drained.
struct Rig {
    WeaponAudio& Wa;
    glm::vec3 Pos{0.0f}, Fwd{0.0f, 0.0f, -1.0f};
    std::vector<float> Master, Pre, Wet;

    void Place(const glm::vec3& p, const glm::vec3& f) {
        Pos = p;
        Fwd = glm::length(f) > 1e-4f ? glm::normalize(f) : glm::vec3(0.0f, 0.0f, -1.0f);
        Wa.SetListener(Pos);
        AudioEngine::SetListener(Pos, Fwd);
    }
    void Clear() {
        Master.clear();
        Pre.clear();
        Wet.clear();
    }
    void Step() {
        Wa.SetListener(Pos);
        Wa.Update((float)kBlock / (float)kRate);
        AudioEngine::RenderOffline(nullptr, kBlock);
        AudioEngine::Update();
        AudioEngine::DrainTaps(Master, Pre, Wet);
    }
    void Run(float seconds) {
        for (int i = 0, n = (int)std::lround(seconds * (float)kRate / (float)kBlock); i < n; ++i) Step();
    }
    // Until the voice has finished (plus a short tail of silence), at most `cap` seconds.
    void RunUntilDone(AudioEngine::SoundHandle h, float cap) {
        const int n = (int)(cap * (float)kRate / (float)kBlock);
        int i = 0;
        for (; i < n && AudioEngine::IsPlaying(h); ++i) Step();
        Run(std::max(0.25f, 0.6f - (float)i * (float)kBlock / (float)kRate)); // at least one full 400 ms loudness window, even for a click

    }
    size_t Frames() const { return Pre.size() / 2; }
};

float LufsMax(const std::vector<float>& s) { return LoudnessMeter::MomentaryMax(s, kRate, 0, s.size() / 2); }

double Energy(const std::vector<float>& s) {
    double e = 0.0;
    for (float v : s) e += (double)v * v;
    return e;
}

const char* Verdict(bool ok) { return ok ? "ok" : "FAIL"; }

// Shots measured as the spec defines its reference: the close + sub + mech layers only (tail / far muted), no pitch or level jitter.
void QuietShotExtras(WeaponAudioProfile& p, bool quiet) {
    const float v = quiet ? 0.0f : 1.0f;
    p.Tail.Set.Volume = p.Far.Set.Volume = v;
    for (WeaponAudioProfile::Layer& l : p.TailClass) l.Set.Volume = v;
    for (WeaponAudioProfile::Layer* l : {&p.Close, &p.Mech, &p.Sub}) l->Set.VolumeJitterDb = 0.0f;
    p.ShotPitchMin = p.ShotPitchMax = 1.0f;
}

} // namespace

int RunAudioTest(int argc, char** argv) {
    std::string sceneArg = "AudioLab", reportPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--audio-test" && i + 1 < argc && argv[i + 1][0] != '-') sceneArg = argv[++i];
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
    }
    if (!reportPath.empty()) {
        s_Report.open(reportPath);
        if (!s_Report.is_open()) std::fprintf(stderr, "--audio-test: cannot write the report '%s'\n", reportPath.c_str());
    }
    const fs::path root = ProjectPaths::Resolve("");
    fs::path scenePath = sceneArg;
    if (scenePath.extension() != ".json") scenePath = root / "scenes" / (sceneArg + ".json");
    else if (scenePath.is_relative() && !fs::exists(scenePath)) scenePath = root / scenePath;
    Out("[audio-test] scene %s\n", scenePath.string().c_str());

    // Loading a scene uploads its textures and meshes: a GL context has to exist. The window stays hidden (it is only shown after a
    // first frame, which never comes here).
    Window window(64, 64, "Tartarus Engine (audio test)");
    World world;
    AssetLibrary assets;
    if (!SceneSerializer::Load(world, assets, scenePath.string(), /*persistMigration=*/false)) {
        Out("[audio-test] FAIL: could not load the scene\n");
        return 1;
    }
    Out("[audio-test] scene loaded\n");
    json manifest, spec;
    const bool haveManifest = ReadJson(root / "assets/Audio/audio_manifest.json", manifest);
    const bool haveSpec = ReadJson((root / "..").lexically_normal() / "tools/audio/recipes/mix.json", spec) && spec.contains("levels");
    float refLufs = 0.0f;
    const bool haveRef = haveManifest && manifest.contains("mix") && Num(manifest["mix"], "reference_lufs_m", refLufs);
    if (!haveRef || !haveSpec) Out("[audio-test] FAIL: no %s: the levels and the ambience are not checked\n", !haveRef ? "mix.reference_lufs_m in the manifest" : "tools/audio/recipes/mix.json");
    Check(haveRef && haveSpec);

    AudioEngine::InitOffline(kRate);
    Out("[audio-test] offline engine up\n");
    AudioEngine::EnableTaps(true);
    WeaponAudio& wa = WeaponAudio::Get();
    wa.Start(world);
    Rig rig{wa};
    const std::vector<ReverbZoneVolume>& zones = wa.Zones().Zones();
    Out("[audio-test] %d zone(s), reference %.2f LUFS-M\n", (int)zones.size(), refLufs);

    // A spot in a zone: its floor + 1.6 m (boxes) or its centre, facing along its longest horizontal axis.
    auto spotIn = [](const ReverbZoneVolume& z, glm::vec3& pos, glm::vec3& fwd, float& room) {
        pos = z.Center;
        fwd = glm::vec3(0.0f, 0.0f, -1.0f);
        room = z.Radius;
        if (z.Shape == 0) {
            pos.y = z.Center.y - z.Extents.y + 1.6f;
            const glm::vec3 ax = glm::normalize(glm::vec3(z.Placed[0])), az = glm::normalize(glm::vec3(z.Placed[2]));
            fwd = z.Extents.x >= z.Extents.z ? ax : az;
            room = std::max(z.Extents.x, z.Extents.z);
        }
    };
    int hall = -1; // the loudest room for the peak: the first indoor large zone, else the first zone
    for (size_t i = 0; i < zones.size(); ++i)
        if (zones[i].Class == SpaceClass::IndoorLarge && hall < 0) hall = (int)i;
    if (hall < 0 && !zones.empty()) hall = 0;
    {
        glm::vec3 p(0.0f, 1.6f, 0.0f), f(0.0f, 0.0f, -1.0f);
        float room = 0.0f;
        if (hall >= 0) spotIn(zones[(size_t)hall], p, f, room);
        rig.Place(p, f);
    }

    // --- 1. levels --------------------------------------------------------------------------------------------------
    wa.Bus().Enabled = false;            // dry: no sends
    wa.Bus().OcclusionEnabled = false;   // and nothing between the listener and the source
    AudioEngine::SetReverbEnabled(false, 0);
    AudioEngine::SetReverbEnabled(false, 1);
    AudioEngine::SetBusVolume(AudioEngine::Bus::Ambient, 0.0f);
    const AudioMixComponent sceneMix = wa.Mix();
    wa.Mix().DuckEnabled = wa.Mix().FocusEnabled = false; // static levels: nothing ducks (part 5 runs the dynamic mix)
    wa.ApplyMix();
    rig.Run(1.2f); // (any bed of the zone fades in silently)

    // --- 0. the tier ladder ----------------------------------------------------------------------------------------------
    if (haveSpec && haveManifest) {
        const json& L = spec["levels"];
        float shotNpc = 0.0f, walk = 0.0f, bedMax = -99.0f;
        Num(L, "npc_shot", shotNpc);
        if (L.contains("step")) Num(L["step"], "walk", walk);
        if (L.contains("ambience"))
            for (auto it = L["ambience"].begin(); it != L["ambience"].end(); ++it)
                if (it.value().is_number()) bedMax = std::max(bedMax, it.value().get<float>());
        Out("\n== tier ladder (spec levels and distance models, dB re the player's shot)\n");
        Out("%-12s %8s %8s %8s %8s %8s\n", "category", "level", "@ref m", "cap", "cap <m", "@20 m");
        bool capsOk = true;
        for (const char* name : {"npc_shot", "flyby", "impact", "body_fall", "npc_step", "npc_gear", "casing"}) {
            const DistanceModel m = ModelOf(manifest, name);
            if (!m.Valid()) {
                Out("%-12s no distance model %s\n", name, Verdict(Check(false)));
                continue;
            }
            // the category's level at its reference distance: the spec's (steps / gear: the player's level + the offset)
            float level = 0.0f;
            if (std::string(name) == "npc_step") level = walk + m.OffsetDb;
            else if (std::string(name) == "npc_gear") { Num(L["elements"], "mag_in", level); level += m.OffsetDb; }
            else if (std::string(name) == "npc_shot") level = shotNpc;
            else Num(L, name, level);
            const float cap = level + m.NearDb, far = level + m.GainDbAt(20.0f) - m.OffsetDb;
            capsOk = capsOk && cap <= -3.0f + 1e-3f;
            Out("%-12s %8.1f %8.0f %8.1f %8.1f %8.1f\n", name, level, m.RefM, cap, m.MinDistance(), far);
        }
        Out("nothing but the player's gun reaches -3 dB, close up %s\n", Verdict(Check(capsOk)));
        const DistanceModel st = ModelOf(manifest, "npc_step");
        const float npcWalk10 = walk + st.GainDbAt(10.0f);
        Out("a soldier's walk at 10 m (%.1f) is 4 dB over the loudest bed (%.1f) %s\n", npcWalk10, bedMax, Verdict(Check(npcWalk10 >= bedMax + 4.0f)));
        Out("the player's walk (%.1f) is 4 dB over the loudest bed %s\n", walk, Verdict(Check(walk >= bedMax + 4.0f)));
        Out("a soldier's walk at 5 m (%.1f) is over the player's own %s\n", walk + st.GainDbAt(5.0f), Verdict(Check(walk + st.GainDbAt(5.0f) > walk)));
    }
    if (haveRef && haveSpec) {
        Out("\n== levels (LUFS-M max, dB re the player's shot; tolerance %.1f dB)\n", kLevelTolDb);
        Out("%-36s %8s %8s %7s\n", "key", "played", "spec", "diff");
        float worst = 0.0f;
        auto report = [&](const std::string& what, float playedLufs, float specDb) {
            const float rel = playedLufs - refLufs, diff = rel - specDb;
            worst = std::max(worst, std::fabs(diff));
            Out("%-36s %8.1f %8.1f %+7.1f %s\n", what.c_str(), rel, specDb, diff, Verdict(Check(std::fabs(diff) <= kLevelTolDb)));
        };
        // The shots: the player's (2D, x player gain) is the 0 dB reference; a soldier's at the npc_shot model's reference distance,
        // and close up (its cap: the reference level + near_db).
        const DistanceModel npcShot = ModelOf(manifest, "npc_shot");
        float npcDb = 0.0f;
        const bool haveNpc = Num(spec["levels"], "npc_shot", npcDb) && npcShot.Valid();
        for (const char* gun : {"ak", "870"}) {
            WeaponAudioProfile* p = wa.Profile(gun);
            if (!p) continue;
            QuietShotExtras(*p, true);
            // A gun may sit off the reference by design (mix.json reference.gun_db: the 12-gauge over the carbine), measured by
            // mixspec into the manifest's mix.gun_offset_db.
            float gunDb = 0.0f;
            if (manifest["mix"].contains("gun_offset_db")) Num(manifest["mix"]["gun_offset_db"], gun, gunDb);
            for (int pass = 0; pass < 3; ++pass) {
                if (pass > 0 && !haveNpc) break;
                const float dist = pass == 1 ? npcShot.RefM : pass == 2 ? std::max(1.0f, 0.5f * npcShot.MinDistance()) : 0.0f;
                float sum = 0.0f;
                const int shots = 3;
                for (int s = 0; s < shots; ++s) {
                    rig.Clear();
                    if (pass > 0) wa.Shot(gun, rig.Pos + rig.Fwd * dist, false, 7u);
                    else wa.Shot(gun, rig.Pos, true);
                    rig.Run(1.6f);
                    sum += LufsMax(rig.Pre);
                }
                char what[64];
                if (pass == 0) std::snprintf(what, sizeof(what), "shot %s (player)", gun);
                else std::snprintf(what, sizeof(what), "shot %s (soldier, %.0f m%s)", gun, dist, pass == 2 ? ", cap" : "");
                report(what, sum / (float)shots, gunDb + (pass == 0 ? 0.0f : pass == 1 ? npcDb : npcDb + npcShot.NearDb));
            }
            QuietShotExtras(*p, false);
        }
        // Every other key: each variant once (up to 3), 2D unless the spec levels it at a distance.
        std::vector<std::string> keys;
        for (const json& e : manifest["files"]) {
            const std::string k = e.value("key", "");
            if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        int skipped = 0;
        for (const std::string& key : keys) {
            float level = 0.0f;
            std::string refName;
            if (key.rfind("snd.amb.", 0) == 0 || !SpecLevel(spec, key, level, refName)) {
                ++skipped;
                continue;
            }
            SoundSet* set = wa.KeySet(key);
            if (!set || set->Files.empty()) {
                Out("%-36s   no files %s\n", key.c_str(), Verdict(Check(false)));
                continue;
            }
            set->VolumeJitterDb = 0.0f;
            set->PitchMin = set->PitchMax = 1.0f;
            const DistanceModel model = ModelOf(manifest, refName);
            const float dist = model.Valid() ? model.RefM : 0.0f;
            const int plays = std::min<int>(3, (int)set->Files.size());
            float sum = 0.0f;
            int n = 0;
            for (int k = 0; k < plays; ++k) {
                rig.Clear();
                const SoundPlayer::Played pl = wa.PlayKeyed(*set, rig.Pos + rig.Fwd * dist, dist <= 0.0f, 1.0f);
                if (!pl.Started) continue;
                if (set->Loop) {
                    rig.Run(1.5f);
                    AudioEngine::Stop(pl.Handle);
                    rig.Run(0.1f);
                } else {
                    rig.RunUntilDone(pl.Handle, 6.0f);
                }
                sum += LufsMax(rig.Pre);
                ++n;
            }
            if (n == 0) {
                Out("%-36s   did not play %s\n", key.c_str(), Verdict(Check(false)));
                continue;
            }
            report(dist > 0.0f ? key + " @" + std::to_string((int)dist) + "m" : key, sum / (float)n, level);
        }
        // A soldier's step and reload: the player's files, in the world at the model's reference distance (level + offset).
        for (const auto& [key, model] : {std::pair<const char*, const char*>{"snd.foley.step_concrete.walk", "npc_step"}, {"snd.ak.mag_in", "npc_gear"}}) {
            float level = 0.0f;
            std::string ignored;
            const DistanceModel m = ModelOf(manifest, model);
            SoundSet* set = wa.KeySet(key);
            if (!m.Valid() || !SpecLevel(spec, key, level, ignored) || !set || set->Files.empty()) continue;
            set->VolumeJitterDb = 0.0f;
            set->PitchMin = set->PitchMax = 1.0f;
            float sum = 0.0f;
            int n = 0;
            for (int k = 0; k < std::min<int>(3, (int)set->Files.size()); ++k) {
                rig.Clear();
                const SoundPlayer::Played pl = wa.PlayKeyed(*set, rig.Pos + rig.Fwd * m.RefM, false, 1.0f);
                if (!pl.Started) continue;
                rig.RunUntilDone(pl.Handle, 6.0f);
                sum += LufsMax(rig.Pre);
                ++n;
            }
            if (n > 0) report(std::string(key) + " (soldier) @" + std::to_string((int)m.RefM) + "m", sum / (float)n, level + m.OffsetDb);
        }
        Out("levels: worst %.1f dB off; %d key(s) not levelled here (shot layers, impulse responses, ambience below)\n", worst, skipped);
    }

    // --- 2. wet / dry per zone ---------------------------------------------------------------------------------------
    wa.Bus().Enabled = true;
    AudioEngine::SetReverbEnabled(true, 0);
    Out("\n== wet / dry per zone (K-weighted reverb return energy re the dry signal, an impact 2 m in front; tolerance %.1f dB)\n", kWetTolDb);
    Out("%-28s %-14s %8s %8s %7s\n", "zone", "class", "wet/dry", "calib", "diff");
    SoundSet* probe = wa.KeySet("snd.impact.concrete");
    if (!probe || probe->Files.empty()) {
        Out("no snd.impact.concrete to excite the rooms with %s\n", Verdict(Check(false)));
    } else {
        probe->VolumeJitterDb = 0.0f;
        probe->PitchMin = probe->PitchMax = 1.0f;
        const float sendDb = 20.0f * std::log10(std::max(wa.SendFor("snd.impact.concrete"), 1e-4f));
        for (size_t i = 0; i < zones.size(); ++i) {
            const ReverbZoneVolume& z = zones[i];
            glm::vec3 p, f;
            float room = 0.0f;
            spotIn(z, p, f, room);
            rig.Place(p, f);
            rig.Run(1.5f); // the reverb glides to this room
            const AudioEngine::ReverbSpec spec0 = wa.CurrentReverb();
            char name[64];
            std::snprintf(name, sizeof(name), "zone %d (%.0f, %.0f, %.0f)", (int)i, z.Center.x, z.Center.y, z.Center.z);
            if (spec0.Count == 0) {
                Out("%-28s %-14s no impulse response %s\n", name, SpaceClassName(z.Class), Verdict(Check(false)));
                continue;
            }
            double calib = 0.0;
            for (int l = 0; l < spec0.Count; ++l) calib += spec0.Layers[l].Weight * std::pow(10.0, spec0.Layers[l].WetDb / 10.0);
            const float calibDb = (float)(10.0 * std::log10(std::max(calib, 1e-12))) + sendDb - wa.Bus().WetTrimDb; // (the trim is the white-noise -> real material correction)
            double wet = 0.0, dry = 0.0, wetRaw = 0.0, dryRaw = 0.0; // K-weighted (as heard) and plain energies
            for (int k = 0; k < 2; ++k) {
                rig.Clear();
                const SoundPlayer::Played pl = wa.PlayKeyed(*probe, rig.Pos + rig.Fwd * std::min(2.0f, 0.5f * room), false, 1.0f);
                if (!pl.Started) continue;
                rig.Run(3.5f); // the event and the longest tail
                std::vector<float> d(rig.Pre.size());
                for (size_t s = 0; s < rig.Pre.size(); ++s) d[s] = rig.Pre[s] - rig.Wet[s];
                wet += LoudnessMeter::KWeightedEnergy(rig.Wet, kRate);
                dry += LoudnessMeter::KWeightedEnergy(d, kRate);
                wetRaw += Energy(rig.Wet);
                dryRaw += Energy(d);
            }
            if (dry <= 0.0) {
                Out("%-28s %-14s the impact did not play %s\n", name, SpaceClassName(z.Class), Verdict(Check(false)));
                continue;
            }
            const float measured = (float)(10.0 * std::log10(std::max(wet / dry, 1e-12)));
            const float raw = (float)(10.0 * std::log10(std::max(wetRaw / std::max(dryRaw, 1e-30), 1e-12)));
            const float diff = measured - calibDb;
            Out("%-28s %-14s %8.1f %8.1f %+7.1f %s  (unweighted %.1f)\n", name, SpaceClassName(z.Class), measured, calibDb, diff,
                Verdict(Check(std::fabs(diff) <= kWetTolDb)), raw);
        }
    }

    // --- 3. ambience ---------------------------------------------------------------------------------------------------
    AudioEngine::SetBusVolume(AudioEngine::Bus::Ambient, 1.0f);
    if (haveRef && haveSpec) {
        Out("\n== ambience beds (LUFS-M max at the zone's centre, dB re the player's shot; tolerance %.1f dB)\n", kLevelTolDb);
        for (size_t i = 0; i < zones.size(); ++i) {
            const ReverbZoneVolume& z = zones[i];
            if (z.Ambience.empty()) continue;
            float level = 0.0f;
            std::string refName;
            if (!SpecLevel(spec, z.Ambience, level, refName)) {
                Out("zone %d: %s has no spec level %s\n", (int)i, z.Ambience.c_str(), Verdict(Check(false)));
                continue;
            }
            level += 20.0f * std::log10(std::max(z.AmbienceVolume, 1e-4f));
            glm::vec3 p, f;
            float room = 0.0f;
            spotIn(z, p, f, room);
            rig.Place(p, f);
            rig.Run(2.5f); // the bed fades in (1 s), the last zone's fades out
            rig.Clear();
            rig.Run(16.5f); // a whole loop: the spec is the file's loudest 400 ms
            const float rel = LufsMax(rig.Pre) - refLufs, diff = rel - level;
            Out("zone %d %-24s %8.1f %8.1f %+7.1f %s\n", (int)i, z.Ambience.c_str(), rel, level, diff, Verdict(Check(std::fabs(diff) <= kLevelTolDb)));
        }
    }

    // --- 4. master peak --------------------------------------------------------------------------------------------------
    {
        glm::vec3 p(0.0f, 1.6f, 0.0f), f(0.0f, 0.0f, -1.0f);
        float room = 10.0f;
        if (hall >= 0) spotIn(zones[(size_t)hall], p, f, room);
        rig.Place(p, f);
        rig.Run(1.5f);
        rig.Clear();
        AudioEngine::TakeLimiterGainReductionDb();
        SoundSet* impact = wa.KeySet("snd.impact.concrete");
        for (int s = 0; s < 30; ++s) { // 30 rounds at 600 rpm, each hitting the far wall, a soldier answering every other
            wa.Shot("ak", rig.Pos, true);
            if (impact && !impact->Files.empty()) wa.PlayKeyed(*impact, rig.Pos + rig.Fwd * std::min(8.0f, 0.8f * room), false, 1.0f);
            if (s % 2 == 0) wa.Shot("ak", rig.Pos + rig.Fwd * 10.0f, false, 7u);
            rig.Run(0.1f);
        }
        rig.Run(2.0f);
        const LimiterSettings lim = AudioEngine::GetMasterLimiter();
        const float master = LoudnessMeter::PeakDb(rig.Master, 0, rig.Master.size() / 2), pre = LoudnessMeter::PeakDb(rig.Pre, 0, rig.Pre.size() / 2);
        const float gr = AudioEngine::TakeLimiterGainReductionDb();
        Out("\n== master peak (a 30-round burst with impacts and a soldier, in the %s)\n", hall >= 0 ? SpaceClassName(zones[(size_t)hall].Class) : "scene");
        Out("into the limiter %.1f dBFS, out %.2f dBFS (ceiling %.1f), deepest gain reduction %.1f dB, LUFS-M max %.1f %s\n", pre, master, lim.CeilingDb, gr,
            LufsMax(rig.Master), Verdict(Check(!lim.Enabled || master <= lim.CeilingDb + 0.05f)));
        const AudioEngine::ReverbStats rs = AudioEngine::GetReverbStats();
        if (rs.Callbacks > 0)
            Out("reverb: %llu blocks, mean %.1f us, max %.1f us per 10 ms block (offline, tail inline)\n", (unsigned long long)rs.Callbacks,
                rs.TotalMicros / (double)rs.Callbacks, rs.MaxMicros);
    }

    // --- 5. firefight -------------------------------------------------------------------------------------------------
    {
        wa.Mix() = sceneMix; // the whole dynamic mix, as the scene has it
        wa.ApplyMix();
        wa.Bus().OcclusionEnabled = true;
        int arena = -1; // an outdoor zone with a bed if there is one (where fights are), else the hall
        for (size_t i = 0; i < zones.size(); ++i)
            if (arena < 0 && !zones[i].Ambience.empty() && (zones[i].Class == SpaceClass::OutdoorUrban || zones[i].Class == SpaceClass::OutdoorOpen)) arena = (int)i;
        if (arena < 0) arena = hall;
        glm::vec3 p(0.0f, 1.6f, 0.0f), f(0.0f, 0.0f, -1.0f);
        float room = 20.0f;
        if (arena >= 0) spotIn(zones[(size_t)arena], p, f, room);
        rig.Place(p, f);
        const glm::vec3 side = glm::normalize(glm::cross(rig.Fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
        rig.Run(2.0f); // the bed fades in
        rig.Clear();
        AudioEngine::TakeLimiterGainReductionDb();
        AudioEngine::TakeGlueGainReductionDb();
        SoundSet* impact = wa.KeySet("snd.impact.concrete");
        SoundSet* npcStep = wa.KeySet("snd.foley.step_concrete.walk");
        SoundSet* ownStep = wa.KeySet("snd.foley.step_concrete.run");
        SoundSet* casing = wa.KeySet("snd.casing.rifle.concrete");
        SoundSet* flyby = wa.KeySet("snd.flyby");
        auto has = [](SoundSet* s) { return s && !s->Files.empty(); };
        const float dt = (float)kBlock / (float)kRate;
        const int blocks = (int)(20.0f / dt);
        int limBlocks = 0, glueBlocks = 0;
        float limMax = 0.0f, glueMax = 0.0f, glueSum = 0.0f, duckMax = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            const float t = (float)b * dt;
            const int tick = b; // 10 ms
            // the player: a 5-round burst (600 rpm) every 2.5 s, impacts on the far wall, casings at the feet; running steps every 0.33 s
            const float inCycle = std::fmod(t, 2.5f);
            if (inCycle < 0.5f && tick % 10 == 0) {
                wa.Shot("ak", rig.Pos, true);
                if (has(impact)) wa.PlayKeyed(*impact, rig.Pos + rig.Fwd * std::min(15.0f, 0.8f * room), false, 1.0f);
                if (has(casing)) wa.PlayKeyed(*casing, rig.Pos + side * 0.8f - glm::vec3(0.0f, 1.5f, 0.0f), false, 1.0f);
            }
            if (has(ownStep) && tick % 33 == 0 && inCycle > 0.6f) wa.PlayKeyed(*ownStep, rig.Pos, true, 1.0f);
            // two soldiers answering: 3-round bursts from 20 m and 35 m, rounds cracking past
            if (std::fmod(t + 1.2f, 2.5f) < 0.3f && tick % 10 == 0) {
                wa.Shot("ak", rig.Pos + rig.Fwd * 20.0f + side * 6.0f, false, 11u);
                if (has(flyby) && tick % 20 == 0) wa.PlayKeyed(*flyby, rig.Pos + side * 1.5f, false, 1.0f);
            }
            if (std::fmod(t + 0.3f, 5.0f) < 0.3f && tick % 10 == 0) wa.Shot("ak", rig.Pos + rig.Fwd * 35.0f - side * 10.0f, false, 12u);
            // a third walking up the flank at 8 m
            if (has(npcStep) && tick % 50 == 0) wa.PlayKeyed(*npcStep, rig.Pos + side * 8.0f, false, 1.0f);
            rig.Step();
            const float lim = AudioEngine::TakeLimiterGainReductionDb(), glue = AudioEngine::TakeGlueGainReductionDb();
            limMax = std::max(limMax, lim);
            glueMax = std::max(glueMax, glue);
            glueSum += glue;
            if (lim > 1.0f) ++limBlocks;
            if (glue > 0.5f) ++glueBlocks;
            duckMax = std::max(duckMax, wa.Ducker().Amount());
        }
        rig.Run(1.0f);
        const float integrated = LoudnessMeter::Integrated(rig.Master, kRate);
        const float lra = LoudnessMeter::ShortTermRange(rig.Master, kRate);
        const LimiterSettings lim = AudioEngine::GetMasterLimiter();
        const float master = LoudnessMeter::PeakDb(rig.Master, 0, rig.Master.size() / 2);
        Out("\n== firefight (20 s in the %s: your bursts, two soldiers answering, flybys, a third walking the flank; ducking / air / glue on)\n",
            arena >= 0 ? SpaceClassName(zones[(size_t)arena].Class) : "scene");
        Out("integrated %.1f LUFS, short-term range %.1f LU, peak %.2f dBFS\n", integrated, lra, master);
        Out("glue: deepest %.1f dB, mean %.1f dB, working (> 0.5 dB) %.0f%% of the time\n", glueMax, glueSum / (float)blocks, 100.0f * (float)glueBlocks / (float)blocks);
        Out("limiter: deepest %.1f dB, working (> 1 dB) %.1f%% of the time %s\n", limMax, 100.0f * (float)limBlocks / (float)blocks,
            Verdict(Check((float)limBlocks <= 0.05f * (float)blocks)));
        Out("the beds ducked to %.1f dB under the fire %s\n", -wa.Mix().DuckBedDb * duckMax, Verdict(Check(!wa.Mix().DuckEnabled || duckMax > 0.9f)));
        Out("peak under the ceiling %s; integrated loudness in [-22, -16] LUFS (a sustained fight) %s\n", Verdict(Check(!lim.Enabled || master <= lim.CeilingDb + 0.05f)),
            Verdict(Check(integrated >= -22.0f && integrated <= -16.0f)));
    }

    wa.Stop();
    AudioEngine::Shutdown();
    Out("\n[audio-test] %d checks, %d failure(s)\n", s_Checks, s_Failures);
    s_Report.close();
    return s_Failures == 0 ? 0 : 1;
}
