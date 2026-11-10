#include "SoundManifest.h"

#include "SoundJson.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>

using json = nlohmann::json;

namespace SoundJson {

float Num(const json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}
bool Flag(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
std::string Str(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace SoundJson

using SoundJson::Flag;
using SoundJson::Num;
using SoundJson::Str;

namespace {

const char* BusName(AudioEngine::Bus b) {
    switch (b) {
    case AudioEngine::Bus::Music: return "music";
    case AudioEngine::Bus::Ambient: return "ambient";
    case AudioEngine::Bus::UI: return "ui";
    case AudioEngine::Bus::Voice: return "voice";
    default: return "sfx";
    }
}
AudioEngine::Bus BusFrom(const std::string& s, AudioEngine::Bus fallback) {
    if (s == "sfx") return AudioEngine::Bus::SFX;
    if (s == "music") return AudioEngine::Bus::Music;
    if (s == "ambient") return AudioEngine::Bus::Ambient;
    if (s == "ui") return AudioEngine::Bus::UI;
    if (s == "voice") return AudioEngine::Bus::Voice;
    return fallback;
}

// "assets/..." as written, or a name inside assets/Audio.
std::string ManifestPath(const std::string& file) {
    if (file.rfind("assets/", 0) == 0 || file.rfind("assets\\", 0) == 0) return file;
    return "assets/Audio/" + file;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Which manifest mix.distance model a set plays by when it is in the world, by key (null: the set's own Min / Max).
const char* DistanceNameForKey(const std::string& key) {
    if (key.rfind("snd.impact.", 0) == 0) return "impact";
    if (key == "snd.body_fall") return "body_fall";
    if (key == "snd.flyby") return "flyby";
    if (key.rfind("snd.casing.", 0) == 0) return "casing";
    if (key.rfind("snd.foley.step_", 0) == 0 || key.rfind("snd.foley.barefoot_", 0) == 0) return "npc_step";
    if (key.rfind("snd.foley.", 0) == 0) return "npc_gear";
    if (key.rfind("snd.amb.", 0) == 0 || key.rfind("snd.ui.", 0) == 0) return nullptr;
    const size_t dot = key.find('.', 4);
    if (key.rfind("snd.", 0) == 0 && dot != std::string::npos) return key.compare(dot + 1, 5, "fire_") == 0 ? "npc_shot" : "npc_gear";
    return nullptr;
}

// The built-in guns' ids, their folders under assets/Audio/Weapons and the word their weapon asset's path carries.
struct GunLayout {
    const char* Id;
    const char* Folder;
    const char* PathWord;
};
constexpr GunLayout kGuns[] = {{kDefaultGunId, "AKS74U", "aks74u"}, {kShotgunGunId, "Remington870", "remington870"},
                               {kRevolverGunId, "Revolver38", "revolver38"}};

} // namespace

namespace SoundJson {

SoundSet ApplySet(const json& j, SoundSet s) {
    if (const auto it = j.find("files"); it != j.end() && it->is_array()) {
        s.Files.clear();
        s.FileAnchorMs.clear(); // (the per-file values belonged to the files this replaces)
        s.FileGainDb.clear();
        s.FilePeakDb.clear();
        for (const auto& f : *it)
            if (f.is_string()) s.Files.push_back(f.get<std::string>());
        s.FilesExplicit = !s.Files.empty();
    }
    if (const auto it = j.find("mixDb"); it != j.end() && it->is_number()) {
        s.HasMixDb = true;
        s.MixDb = it->get<float>();
    }
    s.Volume = Num(j, "volume", s.Volume);
    s.VolumeJitterDb = Num(j, "volumeJitterDb", s.VolumeJitterDb);
    s.PitchMin = Num(j, "pitchMin", s.PitchMin);
    s.PitchMax = Num(j, "pitchMax", s.PitchMax);
    s.Bus = BusFrom(Str(j, "bus"), s.Bus);
    s.MinDistance = std::max(Num(j, "minDistance", s.MinDistance), 0.01f);
    s.MaxDistance = std::max(Num(j, "maxDistance", s.MaxDistance), s.MinDistance);
    if (const std::string r = Str(j, "rolloff"); !r.empty())
        s.Rolloff = r == "linear" ? AudioEngine::Rolloff::Linear : AudioEngine::Rolloff::Logarithmic;
    s.NoImmediateRepeat = Flag(j, "noImmediateRepeat", s.NoImmediateRepeat);
    s.MaxVoices = (int)Num(j, "maxVoices", (float)s.MaxVoices);
    s.StealFadeTime = Num(j, "stealFadeTime", s.StealFadeTime);
    s.Loop = Flag(j, "loop", s.Loop);
    s.ReverbSend = Num(j, "reverbSend", s.ReverbSend);
    if (const auto it = j.find("occlusion"); it != j.end() && it->is_boolean()) s.Occlusion = it->get<bool>() ? 1 : 0;
    return s;
}

json SetToJson(const SoundSet& s) {
    json j;
    j["files"] = s.Files;
    j["volume"] = s.Volume;
    j["volumeJitterDb"] = s.VolumeJitterDb;
    j["pitchMin"] = s.PitchMin;
    j["pitchMax"] = s.PitchMax;
    j["bus"] = BusName(s.Bus);
    j["minDistance"] = s.MinDistance;
    j["maxDistance"] = s.MaxDistance;
    j["rolloff"] = s.Rolloff == AudioEngine::Rolloff::Linear ? "linear" : "log";
    j["noImmediateRepeat"] = s.NoImmediateRepeat;
    j["maxVoices"] = s.MaxVoices;
    j["stealFadeTime"] = s.StealFadeTime;
    j["loop"] = s.Loop;
    j["reverbSend"] = s.ReverbSend;
    if (s.HasMixDb) j["mixDb"] = s.MixDb;
    if (s.Occlusion >= 0) j["occlusion"] = s.Occlusion > 0;
    return j;
}

} // namespace SoundJson

// --- SoundSet ----------------------------------------------------------------------------------

SoundSet SoundSet::FromJson(const std::string& key, const std::string& text, const SoundSet& base) {
    SoundSet s = base;
    s.Key = key;
    const json j = json::parse(text, nullptr, false);
    if (j.is_object()) s = SoundJson::ApplySet(j, s);
    s.Key = key;
    return s;
}

std::string SoundSet::ToJson() const { return SoundJson::SetToJson(*this).dump(2); }

float SoundSet::GainLin(size_t i) const {
    if (HasMixDb) return std::pow(10.0f, MixDb / 20.0f);
    return i < FileGainDb.size() ? std::pow(10.0f, FileGainDb[i] / 20.0f) : 1.0f;
}

bool SoundSet::AdoptFiles(const SoundSet& fresh, bool takeLoop) {
    if (fresh.Files.empty() || FilesExplicit) return false;
    Files = fresh.Files;
    FileAnchorMs = fresh.FileAnchorMs;
    FileGainDb = fresh.FileGainDb;
    FilePeakDb = fresh.FilePeakDb;
    FileLufs = fresh.FileLufs;
    if (takeLoop && fresh.Loop) Loop = true;
    return true;
}

// --- manifest ----------------------------------------------------------------------------------

bool SoundManifest::FromJson(const std::string& text, SoundManifest& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (error) *error = "audio manifest isn't valid json";
        return false;
    }
    out.Entries.clear();
    out.Distance.clear();
    out.ShotOverReferenceDb = 0.0f;
    if (j.is_object())
        if (const auto mix = j.find("mix"); mix != j.end() && mix->is_object()) {
            if (const auto d = mix->find("distance"); d != mix->end() && d->is_object())
                for (auto r = d->begin(); r != d->end(); ++r) {
                    if (!r.value().is_object()) continue;
                    DistanceModel m;
                    m.RefM = Num(r.value(), "ref_m", 0.0f);
                    m.SlopeDb = Num(r.value(), "slope_db", 6.0f);
                    m.NearDb = Num(r.value(), "near_db", 0.0f);
                    m.MaxM = Num(r.value(), "max_m", 40.0f);
                    m.OffsetDb = Num(r.value(), "offset_db", 0.0f);
                    if (m.Valid()) out.Distance[r.key()] = m;
                }
            out.ShotOverReferenceDb = Num(*mix, "shot_lufs_m", 0.0f) - Num(*mix, "reference_lufs_m", 0.0f);
            out.HasReference = mix->contains("reference_lufs_m") && (*mix)["reference_lufs_m"].is_number();
            out.ReferenceLufs = Num(*mix, "reference_lufs_m", 0.0f);
        }
    auto add = [&](const std::string& file, const json& e) {
        if (!e.is_object() || file.empty()) return;
        SoundManifestEntry m;
        m.File = ManifestPath(file);
        m.Key = Str(e, "key");
        m.Layer = Str(e, "layer");
        m.AnchorMs = Num(e, "anchor_ms", 0.0f);
        m.MixDb = Num(e, "mix_db", 0.0f);
        m.PeakDb = Num(e, "true_peak_dbtp", -3.0f);
        m.HasLufs = e.contains("lufs_m_max") && e["lufs_m_max"].is_number();
        m.LufsM = Num(e, "lufs_m_max", 0.0f);
        m.PreDelayMs = Num(e, "predelay_ms", 0.0f);
        m.Loop = Flag(e, "loop", false);
        if (!m.Key.empty()) out.Entries.push_back(std::move(m));
    };
    const json* list = &j;
    if (j.is_object()) {
        if (const auto it = j.find("files"); it != j.end()) list = &*it;
        else if (const auto it2 = j.find("entries"); it2 != j.end()) list = &*it2;
    }
    if (list->is_array()) {
        for (const auto& e : *list) add(e.is_object() ? Str(e, "file") : std::string(), e);
    } else if (list->is_object()) {
        for (auto it = list->begin(); it != list->end(); ++it) add(it.key(), it.value()); // file -> {key, category, layer}
    }
    std::sort(out.Entries.begin(), out.Entries.end(), [](const SoundManifestEntry& a, const SoundManifestEntry& b) { return a.File < b.File; });
    return true;
}

const SoundManifestEntry* SoundManifest::FirstFor(const std::string& key) const {
    for (const SoundManifestEntry& e : Entries)
        if (e.Key == key) return &e;
    return nullptr;
}

std::vector<std::string> SoundManifest::FilesFor(const std::string& key) const {
    std::vector<std::string> out;
    for (const SoundManifestEntry& e : Entries)
        if (e.Key == key) out.push_back(e.File);
    return out;
}

std::vector<std::string> SoundManifest::Keys() const {
    std::set<std::string> k;
    for (const SoundManifestEntry& e : Entries) k.insert(e.Key);
    return {k.begin(), k.end()};
}

void SoundManifest::FillSet(SoundSet& set, const std::string& root) const {
    if (const char* name = DistanceNameForKey(set.Key)) set.Distance = DistanceFor(name);
    if (!set.Files.empty()) return;
    set.FileAnchorMs.clear();
    set.FileGainDb.clear();
    set.FilePeakDb.clear();
    set.FileLufs.clear();
    for (const SoundManifestEntry& e : Entries)
        if (e.Key == set.Key) {
            set.Files.push_back(e.File);
            set.FileAnchorMs.push_back(e.AnchorMs);
            set.FileGainDb.push_back(e.MixDb);
            set.FilePeakDb.push_back(e.PeakDb);
            set.FileLufs.push_back(e.HasLufs ? e.LufsM : -200.0f);
            if (e.Loop) set.Loop = true;
        }
    if (set.Files.empty() && !root.empty()) set.Files = SoundFilesByLayout(root, set.Key);
}

std::string WeaponAudioGunFolder(const std::string& gunId) {
    for (const GunLayout& g : kGuns)
        if (gunId == g.Id) return g.Folder;
    return gunId;
}

std::string WeaponAudioGunId(const std::string& weaponPath) {
    const std::string lower = Lower(weaponPath);
    for (const GunLayout& g : kGuns)
        if (lower.find(g.PathWord) != std::string::npos) return g.Id;
    std::string stem = std::filesystem::path(weaponPath).stem().string();
    return Lower(stem);
}

std::string SoundKeyFilePrefix(const std::string& key) {
    if (key.rfind("snd.", 0) != 0) return {};
    const std::string rest = key.substr(4);
    if (rest.rfind("foley.", 0) == 0) {
        const std::string r2 = rest.substr(6);
        const size_t dot = r2.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= r2.size()) return {};
        return "assets/Audio/Foley/" + r2.substr(0, dot) + "/" + r2.substr(dot + 1) + "_";
    }
    const size_t dot = rest.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= rest.size()) return {};
    return "assets/Audio/Weapons/" + WeaponAudioGunFolder(rest.substr(0, dot)) + "/" + rest.substr(dot + 1) + "_";
}

std::vector<std::string> SoundFilesByLayout(const std::string& root, const std::string& key) {
    std::vector<std::string> out;
    const std::string prefix = SoundKeyFilePrefix(key);
    if (prefix.empty()) return out;
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(root) / fs::path(prefix).parent_path();
    const std::string stem = fs::path(prefix).filename().string(); // "mag_out_"
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        const std::string ext = Lower(e.path().extension().string());
        if (name.rfind(stem, 0) != 0 || (ext != ".wav" && ext != ".ogg" && ext != ".mp3" && ext != ".flac")) continue;
        // "mag_out_1.wav", not "mag_out_long_1.wav": the rest of the stem is the variant number.
        const std::string num = e.path().stem().string().substr(stem.size());
        if (num.empty() || !std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; })) continue;
        out.push_back(std::string(fs::path(prefix).parent_path().generic_string()) + "/" + name);
    }
    std::sort(out.begin(), out.end());
    return out;
}
