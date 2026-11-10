#pragma once

#include "AudioEngine.h"
#include "AudioMix.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// The audio manifest and the sets it fills. A SoundSet is a bag of variants plus how to play them (volume, jitter, pitch, bus, 3D
// range, round robin, voice cap); the sets are filled from the manifest (assets/Audio/audio_manifest.json, written by tools/audio) by
// key, so recording a new take is a file drop, never a code change.

// Gun ids. The default gun is the one every fallback uses (the reverb's environment settings, the gear sounds' reach); the built-in
// ones always have a profile.
inline constexpr const char* kDefaultGunId = "ak";
inline constexpr const char* kShotgunGunId = "870";
inline constexpr const char* kRevolverGunId = "revolver38";
inline constexpr const char* kBuiltInGunIds[] = {kDefaultGunId, kShotgunGunId, kRevolverGunId};

// One playable sound: its variants and how it is played.
struct SoundSet {
    std::string Key;                       // "snd.ak.mag_out" - also the voice pool the cap counts in
    std::vector<std::string> Files;        // project-relative paths; picked round robin, never the same twice running
    float Volume = 1.0f;                   // linear gain
    float VolumeJitterDb = 0.0f;           // +- dB of random gain per play
    float PitchMin = 1.0f, PitchMax = 1.0f;// random playback rate per play
    AudioEngine::Bus Bus = AudioEngine::Bus::SFX;
    float MinDistance = 2.0f, MaxDistance = 40.0f; // 3D plays: full volume inside Min, rolled off to Max
    AudioEngine::Rolloff Rolloff = AudioEngine::Rolloff::Logarithmic;
    bool NoImmediateRepeat = true;         // round robin: a variant never plays twice in a row (with 2+ variants)
    int MaxVoices = 8;                     // simultaneous voices of this set (0 = unlimited); the oldest is stolen
    float StealFadeTime = 0.03f;           // seconds the stolen voice fades out over (0 = cut)
    bool Loop = false;
    // The reverb bus: this set's send level (linear, post fader), or -1 = by category (the Reverb Bus component's table: gun
    // tails 0, shot layers a little, foley / gear / impacts / casings more). Occlusion: -1 = by category (3D voices on),
    // 0 = off, 1 = a low-pass when geometry stands between the listener and the source.
    float ReverbSend = -1.0f;
    int Occlusion = -1;
    // Per file (parallel to Files; empty = zeros / defaults), from the manifest: where the contact transient sits in the file
    // (ms), the mix level (dB) and the true peak (dBTP). Variants of one key differ, so these are used per play, not per key.
    std::vector<float> FileAnchorMs, FileGainDb, FilePeakDb;
    std::vector<float> FileLufs;           // the files' LUFS-M max (empty: unknown), for the duck's key
    float AnchorMs(size_t i) const { return i < FileAnchorMs.size() ? FileAnchorMs[i] : 0.0f; }
    // The level contract: a voice plays at Volume x 10^(mix_db / 20) (GainLin), nothing else loudness-related on top. mix_db is the
    // manifest's (per file), or the set's own "mixDb" from a Data File (HasMixDb), which wins.
    bool HasMixDb = false;
    float MixDb = 0.0f;
    float GainLin(size_t i) const;
    bool FilesExplicit = false;            // the files came from a Data File: the manifest does not replace them
    // 3D plays of a category the mix spec places in the world (manifest mix.distance, by key): the level holds at the model's
    // reference distance, capped close up, its own slope beyond; MinDistance / MaxDistance / Rolloff above are then not used.
    DistanceModel Distance;

    static SoundSet FromJson(const std::string& key, const std::string& text, const SoundSet& base = SoundSet{});
    std::string ToJson() const;
    // Takes the manifest-resolved files of `fresh` (files and their per-file values) unless this set's files are explicit (a Data
    // File's own win) or `fresh` found none. `takeLoop` also takes its loop flag. True when it took them.
    bool AdoptFiles(const SoundSet& fresh, bool takeLoop);
};

// One entry of tools/audio's manifest (assets/Audio/audio_manifest.json): file, key, category, layer.
struct SoundManifestEntry {
    std::string File, Key, Layer;
    float AnchorMs = 0.0f, MixDb = 0.0f, PeakDb = -3.0f;
    float LufsM = 0.0f; bool HasLufs = false; // lufs_m_max: the file's loudness (the mix's level of a voice is this + its gain - the reference)
    float PreDelayMs = 0.0f; // impulse responses (layer "ir", key ir.<class>)
    bool Loop = false;
};
struct SoundManifest {
    std::vector<SoundManifestEntry> Entries;
    // Top-level mix.distance (tools/audio/recipes/mix.json "distance"): npc_shot, impact, body_fall, npc_step, npc_gear, casing, flyby.
    std::map<std::string, DistanceModel> Distance;
    float ShotOverReferenceDb = 0.0f;  // mix.shot_lufs_m - mix.reference_lufs_m: a shot layer's mix_db is relative to the unscaled shot
    float ReferenceLufs = 0.0f; bool HasReference = false; // mix.reference_lufs_m: the player's shot as played (0 dB of the mix)
    DistanceModel DistanceFor(const std::string& name) const { const auto it = Distance.find(name); return it == Distance.end() ? DistanceModel{} : it->second; }
    float DistanceRef(const std::string& name) const { return DistanceFor(name).RefM; }
    const SoundManifestEntry* FirstFor(const std::string& key) const; // the first variant of `key` (file-name order), null when none
    static bool FromJson(const std::string& text, SoundManifest& out, std::string* error = nullptr);
    // Every variant of `key`, in file-name order (empty when none).
    std::vector<std::string> FilesFor(const std::string& key) const;
    std::vector<std::string> Keys() const;
    // Resolves a set's files from the manifest (or, failing that, the file layout under `root`) when it has none of its own, and its
    // distance model by key.
    void FillSet(SoundSet& set, const std::string& root) const;
};
// "snd.ak.mag_out" -> "assets/Audio/Weapons/AKS74U/mag_out_" (the variant prefix of the contract's file layout);
// "snd.foley.concrete.walk" -> "assets/Audio/Foley/concrete/walk_". Empty when the key doesn't parse.
std::string SoundKeyFilePrefix(const std::string& key);
// The contract's directory layout as a fallback when the manifest has no entry: every <prefix>*.wav
// beside `root` (the project folder), sorted.
std::vector<std::string> SoundFilesByLayout(const std::string& root, const std::string& key);
// The gun a key or a weapon asset belongs to: "ak" for ".../AKS74U/...", "870" for Remington870,
// otherwise the lower-cased file stem.
std::string WeaponAudioGunId(const std::string& weaponPath);
std::string WeaponAudioGunFolder(const std::string& gunId);
