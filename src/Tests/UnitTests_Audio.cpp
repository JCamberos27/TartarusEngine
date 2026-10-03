#include "UnitTestSupport.h"

#include "../Game/Audio/FoleyAudio.h"
#include "../Game/Audio/EnvironmentProbe.h"
#include "../Game/Audio/ReverbZones.h"
#include "../Game/Audio/WeaponAudio.h"
#include "../Game/ComponentRegistry.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

// Unit tests for the engine audio wiring (Game/Audio: SoundSet playback rules, weapon audio profiles, foley, the manifest).

namespace {

// Records every voice instead of playing it; a voice plays until Age() runs its life out or it is stopped.
struct FakeBackend : SoundBackend {
    struct V {
        SoundVoice Voice;
        bool Playing = true;
        float Volume = 0.0f;
        float Life = 1e9f;
    };
    std::vector<V> Voices;
    float DefaultLife = 1e9f;
    AudioEngine::SoundHandle Start(const SoundVoice& v) override {
        V x;
        x.Voice = v;
        x.Volume = v.Volume;
        x.Life = DefaultLife;
        Voices.push_back(x);
        return (AudioEngine::SoundHandle)Voices.size();
    }
    void Stop(AudioEngine::SoundHandle h) override {
        if (h >= 1 && h <= Voices.size()) Voices[h - 1].Playing = false;
    }
    void SetVolume(AudioEngine::SoundHandle h, float v) override {
        if (h >= 1 && h <= Voices.size()) Voices[h - 1].Volume = v;
    }
    bool IsPlaying(AudioEngine::SoundHandle h) override { return h >= 1 && h <= Voices.size() && Voices[h - 1].Playing; }
    void Age(float dt) {
        for (V& v : Voices)
            if (v.Playing && (v.Life -= dt) <= 0.0f) v.Playing = false;
    }
    int Live() const { return (int)std::count_if(Voices.begin(), Voices.end(), [](const V& v) { return v.Playing; }); }
    int Started(const std::string& file) const {
        return (int)std::count_if(Voices.begin(), Voices.end(), [&](const V& v) { return v.Voice.File == file; });
    }
};

SoundSet MakeSet(const char* key, std::vector<std::string> files, int maxVoices = 8, float fade = 0.0f) {
    SoundSet s;
    s.Key = key;
    s.Files = std::move(files);
    s.MaxVoices = maxVoices;
    s.StealFadeTime = fade;
    return s;
}

void TestSoundSetRoundRobinNeverRepeats() {
    FakeBackend be;
    SoundPlayer player(&be);
    player.Seed(7u);
    const SoundSet three = MakeSet("t.rr3", {"a.wav", "b.wav", "c.wav"}, 0);
    std::string last;
    std::set<std::string> seen;
    bool repeated = false;
    for (int i = 0; i < 300; ++i) {
        const SoundPlayer::Played p = player.Play(three, {});
        if (p.File == last) repeated = true;
        last = p.File;
        seen.insert(p.File);
    }
    CHECK(!repeated);
    CHECK(seen.size() == 3); // every variant gets used
    // Two variants must strictly alternate.
    const SoundSet two = MakeSet("t.rr2", {"x.wav", "y.wav"}, 0);
    std::string prev = player.Play(two, {}).File;
    bool alternates = true;
    for (int i = 0; i < 50; ++i) {
        const std::string f = player.Play(two, {}).File;
        if (f == prev) alternates = false;
        prev = f;
    }
    CHECK(alternates);
    // A single file just plays; round robin off may repeat.
    SoundSet one = MakeSet("t.rr1", {"only.wav"}, 0);
    CHECK(player.Play(one, {}).File == "only.wav" && player.Play(one, {}).File == "only.wav");
    SoundSet free = MakeSet("t.rrfree", {"p.wav", "q.wav"}, 0);
    free.NoImmediateRepeat = false;
    int repeats = 0;
    std::string f0;
    for (int i = 0; i < 200; ++i) {
        const std::string f = player.Play(free, {}).File;
        if (f == f0) ++repeats;
        f0 = f;
    }
    CHECK(repeats > 0);
    // Each set has its own round robin: interleaving two sets doesn't break either.
    const SoundSet other = MakeSet("t.other", {"u.wav", "v.wav", "w.wav"}, 0);
    last.clear();
    repeated = false;
    for (int i = 0; i < 100; ++i) {
        player.Play(other, {});
        const std::string f = player.Play(three, {}).File;
        if (f == last) repeated = true;
        last = f;
    }
    CHECK(!repeated);
}

void TestSoundSetStealOldest() {
    FakeBackend be;
    SoundPlayer player(&be);
    const SoundSet cut = MakeSet("t.steal", {"a.wav", "b.wav"}, 3, 0.0f);
    std::vector<AudioEngine::SoundHandle> handles;
    for (int i = 0; i < 5; ++i) handles.push_back(player.Play(cut, {}).Handle);
    CHECK(player.Voices("t.steal") == 3);
    CHECK(be.Live() == 3);
    // The two oldest were the ones that went.
    CHECK(!be.IsPlaying(handles[0]) && !be.IsPlaying(handles[1]));
    CHECK(be.IsPlaying(handles[2]) && be.IsPlaying(handles[3]) && be.IsPlaying(handles[4]));
    // Another pool is untouched.
    const SoundSet other = MakeSet("t.steal2", {"c.wav"}, 3, 0.0f);
    player.Play(other, {});
    CHECK(player.Voices("t.steal") == 3 && player.Voices("t.steal2") == 1);

    // With a fade the stolen voice rings on while it fades, then goes; it no longer counts toward the cap.
    FakeBackend be2;
    SoundPlayer p2(&be2);
    const SoundSet fade = MakeSet("t.fade", {"a.wav", "b.wav"}, 2, 0.2f);
    const AudioEngine::SoundHandle first = p2.Play(fade, {}).Handle;
    p2.Play(fade, {});
    p2.Play(fade, {}); // steals `first`
    CHECK(p2.Voices("t.fade") == 2);
    CHECK(p2.AudibleVoices("t.fade") == 3);
    CHECK(be2.IsPlaying(first));
    p2.Update(0.1f);
    CHECK(be2.IsPlaying(first) && be2.Voices[first - 1].Volume < 0.75f && be2.Voices[first - 1].Volume > 0.25f); // half faded
    p2.Update(0.15f);
    CHECK(!be2.IsPlaying(first));
    CHECK(p2.AudibleVoices("t.fade") == 2);
    // Finished voices are forgotten, so the cap never counts the dead.
    be2.DefaultLife = 0.05f;
    SoundPlayer p3(&be2);
    const SoundSet quick = MakeSet("t.quick", {"a.wav"}, 1, 0.0f);
    const AudioEngine::SoundHandle h1 = p3.Play(quick, {}).Handle;
    be2.Age(0.1f);
    const AudioEngine::SoundHandle h2 = p3.Play(quick, {}).Handle;
    CHECK(h1 != h2 && !be2.IsPlaying(h1) && be2.IsPlaying(h2));
}

void TestSoundSetJitterPitchAndRequest() {
    FakeBackend be;
    SoundPlayer player(&be);
    player.Seed(99u);
    SoundPlayer::Limiter off;
    off.Enabled = false;
    player.SetLimiter(off); // (the jitter is what is measured here)
    SoundSet s = MakeSet("t.jit", {"a.wav"}, 0);
    s.Volume = 0.5f;
    s.VolumeJitterDb = 3.0f;
    s.PitchMin = 0.9f;
    s.PitchMax = 1.1f;
    s.Bus = AudioEngine::Bus::UI;
    float minV = 9.0f, maxV = 0.0f, minP = 9.0f, maxP = 0.0f;
    for (int i = 0; i < 200; ++i) {
        SoundPlayer::Request r;
        r.Gain = 2.0f;
        const SoundPlayer::Played p = player.Play(s, r);
        minV = std::min(minV, p.Volume);
        maxV = std::max(maxV, p.Volume);
        minP = std::min(minP, p.Pitch);
        maxP = std::max(maxP, p.Pitch);
    }
    const float lo = 1.0f * std::pow(10.0f, -3.0f / 20.0f), hi = 1.0f * std::pow(10.0f, 3.0f / 20.0f);
    CHECK(minV >= lo - 1e-4f && maxV <= hi + 1e-4f);
    CHECK(maxV - minV > 0.2f); // it really jitters
    CHECK(minP >= 0.9f - 1e-4f && maxP <= 1.1f + 1e-4f && maxP - minP > 0.1f);
    CHECK(be.Voices.back().Voice.Bus == AudioEngine::Bus::UI);
    // 3D requests carry the position and the set's range.
    SoundSet spatial = MakeSet("t.3d", {"a.wav"}, 0);
    spatial.MinDistance = 3.0f;
    spatial.MaxDistance = 77.0f;
    SoundPlayer::Request r3;
    r3.At2D = false;
    r3.Position = glm::vec3(1.0f, 2.0f, 3.0f);
    player.Play(spatial, r3);
    const SoundVoice& v = be.Voices.back().Voice;
    CHECK(v.Spatial && v.Position == r3.Position && v.MinDistance == 3.0f && v.MaxDistance == 77.0f);
    player.Play(spatial, {});
    CHECK(!be.Voices.back().Voice.Spatial);
    // A set with no files starts nothing but still reports the play to the log.
    int logged = 0;
    player.SetLog([&](double, const std::string& key, const std::string& file, int, const SoundPlayer::Request&, float, float, const SoundVoice*) {
        if (key == "t.empty" && file.empty()) ++logged;
    });
    const size_t before = be.Voices.size();
    const SoundPlayer::Played none = player.Play(MakeSet("t.empty", {}), {});
    CHECK(!none.Started && be.Voices.size() == before && logged == 1);
}

void TestSoundSetJsonRoundTrip() {
    SoundSet s = MakeSet("snd.test.x", {"assets/a.wav", "assets/b.wav"}, 5, 0.07f);
    s.Volume = 0.6f;
    s.VolumeJitterDb = 2.0f;
    s.PitchMin = 0.8f;
    s.PitchMax = 1.3f;
    s.Bus = AudioEngine::Bus::Ambient;
    s.MinDistance = 4.0f;
    s.MaxDistance = 55.0f;
    s.Rolloff = AudioEngine::Rolloff::Linear;
    s.NoImmediateRepeat = false;
    s.Loop = true;
    const SoundSet back = SoundSet::FromJson("snd.test.x", s.ToJson());
    CHECK(back.Files == s.Files && back.Volume == s.Volume && back.VolumeJitterDb == s.VolumeJitterDb);
    CHECK(back.PitchMin == s.PitchMin && back.PitchMax == s.PitchMax && back.Bus == s.Bus);
    CHECK(back.MinDistance == s.MinDistance && back.MaxDistance == s.MaxDistance && back.Rolloff == s.Rolloff);
    CHECK(!back.NoImmediateRepeat && back.MaxVoices == 5 && back.StealFadeTime == s.StealFadeTime && back.Loop);
    // A partial json only overrides what it names.
    const SoundSet part = SoundSet::FromJson("k", R"({"volume":0.25})", s);
    CHECK(part.Volume == 0.25f && part.Files == s.Files && part.MaxVoices == 5);
}

void TestDistanceBlendWeights() {
    const WeaponAudioProfile ak = WeaponAudioProfile::Default("ak");
    // Close: full to 18 m, gone by 43 m, linear between.
    CHECK(ak.Close.Curve.Weight(0.0f) == 1.0f && ak.Close.Curve.Weight(18.0f) == 1.0f);
    CHECK(std::fabs(ak.Close.Curve.Weight(30.5f) - 0.5f) < 1e-4f);
    CHECK(ak.Close.Curve.Weight(43.0f) == 0.0f && ak.Close.Curve.Weight(500.0f) == 0.0f);
    // Far: a tenth up close (it is the field's echo), full past the crack.
    CHECK(std::fabs(ak.Far.Curve.Weight(5.0f) - 0.1f) < 1e-4f);
    CHECK(std::fabs(ak.Far.Curve.Weight(30.5f) - 0.55f) < 1e-4f);
    CHECK(std::fabs(ak.Far.Curve.Weight(100.0f) - 1.0f) < 1e-4f);
    // The tail never drops out; the sub carries further than the mech.
    CHECK(ak.Tail.Curve.Weight(200.0f) > 0.5f);
    CHECK(ak.Sub.Curve.Weight(40.0f) > ak.Mech.Curve.Weight(40.0f));

    // Through Shot(): a soldier's rifle.
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    wa.SetListener(glm::vec3(0.0f));
    SoundPlayer::Limiter off;
    off.Enabled = false;
    wa.Player().SetLimiter(off);
    auto volumeOf = [&](const char* file) {
        float v = -1.0f;
        for (const auto& x : be.Voices)
            if (x.Voice.File == file) v = x.Voice.Volume;
        return v;
    };
    wa.Shot("ak", glm::vec3(10.0f, 0.0f, 0.0f), false);
    CHECK(volumeOf("assets/Audio/Combat/ak_shot.wav") > 0.7f || volumeOf("assets/Audio/Combat/ak_shot_b.wav") > 0.7f); // the crack, full (+-1 dB)
    CHECK(volumeOf("assets/Audio/Combat/ak_shot_far.wav") > 0.05f && volumeOf("assets/Audio/Combat/ak_shot_far.wav") < 0.15f);
    be.Voices.clear();
    wa.Update(1.0f); // a new burst
    wa.Shot("ak", glm::vec3(70.0f, 0.0f, 0.0f), false);
    CHECK(volumeOf("assets/Audio/Combat/ak_shot.wav") < 0.0f && volumeOf("assets/Audio/Combat/ak_shot_b.wav") < 0.0f); // no crack at 70 m
    CHECK(volumeOf("assets/Audio/Combat/ak_shot_far.wav") > 0.7f);
    CHECK(be.Voices.back().Voice.Spatial);
    be.Voices.clear();
    // The player's own gun: 2D, no distant layer, at the player gain.
    wa.Shot("ak", glm::vec3(0.0f), true);
    CHECK(be.Voices.size() == 1 && !be.Voices[0].Voice.Spatial && be.Voices[0].Voice.Volume < 0.75f * 1.13f + 1e-3f && be.Voices[0].Voice.Volume > 0.75f * 0.88f);
    wa.Stop();
}

void TestAutofireVoiceCap() {
    FakeBackend be;
    be.DefaultLife = 1.5f; // a tail rings 1.5 s
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    WeaponAudioProfile* p = wa.Profile("ak");
    CHECK(p != nullptr);
    p->Tail.Every = 1; // (the decimation has its own test)
    p->Tail.Set.Files = {"tail_1.wav", "tail_2.wav"};
    p->Mech.Set.Files = {"mech_1.wav"};
    p->Sub.Set.Files = {"sub_1.wav"};
    const std::string tailKey = p->Tail.Set.Key;
    int peakTail = 0, peakAudible = 0, peakClose = 0;
    float peakTailGain = 0.0f;
    // 900 rpm for six seconds: 90 shots, every 1/15 s.
    const float dt = 1.0f / 15.0f;
    for (int i = 0; i < 90; ++i) {
        wa.Shot("ak", glm::vec3(0.0f), true);
        wa.Update(dt);
        be.Age(dt);
        peakTail = std::max(peakTail, wa.Player().Voices(tailKey));
        peakAudible = std::max(peakAudible, wa.Player().AudibleVoices(tailKey));
        peakClose = std::max(peakClose, wa.Player().Voices(p->Close.Set.Key));
        float sum = 0.0f;
        for (const auto& v : be.Voices)
            if (v.Playing && (v.Voice.File == "tail_1.wav" || v.Voice.File == "tail_2.wav")) sum += v.Volume;
        peakTailGain = std::max(peakTailGain, sum);
    }
    CHECK(peakTail <= p->Tail.Set.MaxVoices); // the cap holds ...
    CHECK(peakTail == p->Tail.Set.MaxVoices); // ... and is reached: the test really overlapped them
    CHECK(peakAudible <= p->Tail.Set.MaxVoices + 4); // stolen ones are fading out, a few at most
    CHECK(peakClose <= p->Close.Set.MaxVoices);
    // Ducking: three tails at full gain would be 3 x the player gain; they sum to well under that.
    CHECK(peakTailGain < 0.75f * 3.0f * 0.85f);
    // Every shot played its short layers: close, mech and sub, none dropped by the cap (steal, not refuse).
    CHECK(be.Started("mech_1.wav") == 90 && be.Started("sub_1.wav") == 90);
    // A minimum tail interval spaces them: 10 shots in 0.5 s with 0.25 s between tails = 2-3 tails.
    be.Voices.clear();
    p->TailMinInterval = 0.25f;
    wa.Update(1.0f);
    for (int i = 0; i < 10; ++i) {
        wa.Shot("ak", glm::vec3(0.0f), true);
        wa.Update(0.05f);
    }
    const int tails = be.Started("tail_1.wav") + be.Started("tail_2.wav");
    CHECK(tails >= 2 && tails <= 3);
    wa.Stop();
}

void TestAnimEventPlaysItsSet() {
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    WeaponAudioProfile* ak = wa.Profile("ak");
    WeaponAudioProfile* sg = wa.Profile("870");
    CHECK(ak && sg);
    ak->Events["mag_out"] = MakeSet("snd.ak.mag_out", {"assets/ak_magout_1.wav"});
    sg->Events["pump_back"] = MakeSet("snd.870.pump_back", {"assets/pump_back_1.wav"});
    // The first-person player's gun: 2D, the key picks the gun's set.
    CHECK(wa.PlayEvent("snd.ak.mag_out", "", glm::vec3(0.0f), true));
    CHECK(be.Voices.size() == 1 && be.Voices[0].Voice.File == "assets/ak_magout_1.wav" && !be.Voices[0].Voice.Spatial);
    // A soldier's: 3D at his gun.
    CHECK(wa.PlayEvent("snd.870.pump_back", "", glm::vec3(4.0f, 1.0f, -2.0f), false));
    CHECK(be.Voices.size() == 2 && be.Voices[1].Voice.File == "assets/pump_back_1.wav" && be.Voices[1].Voice.Spatial);
    CHECK(be.Voices[1].Voice.Position == glm::vec3(4.0f, 1.0f, -2.0f));
    // Not a weapon key: ignored by the parse; a gun with the key but no recorded files plays nothing (and doesn't throw).
    CHECK(!wa.PlayEvent("Shot", "", glm::vec3(0.0f), true));
    CHECK(wa.PlayEvent("snd.ak.not_recorded_yet", "", glm::vec3(0.0f), true) && be.Voices.size() == 2);
    // Foley keys on a weapon's animation play the foley set.
    SoundSet* cloth = wa.FoleySet("cloth", "regrip");
    cloth->Files = {"assets/cloth_regrip_1.wav"};
    CHECK(wa.PlayEvent("snd.foley.cloth.regrip", "", glm::vec3(0.0f), true) && be.Voices.back().Voice.File == "assets/cloth_regrip_1.wav");
    // Disabled gun: silent.
    ak->Enabled = false;
    CHECK(!wa.PlayEvent("snd.ak.mag_out", "", glm::vec3(0.0f), true));
    // Every play is in the history, in order, with its 2D / 3D.
    const auto& h = wa.History();
    CHECK(h.size() >= 4 && h[0].Key == "snd.ak.mag_out" && h[0].At2D && h[1].Key == "snd.870.pump_back" && !h[1].At2D);
    std::string gun, element;
    CHECK(WeaponAudio::ParseKey("snd.870.shell_insert", gun, element) && gun == "870" && element == "shell_insert");
    CHECK(!WeaponAudio::ParseKey("Shot", gun, element) && !WeaponAudio::ParseKey("snd.", gun, element) && !WeaponAudio::ParseKey("snd.ak", gun, element));
    CHECK(WeaponAudioGunId("assets/Weapons/AKS74U/AKS74U.fpsanim") == "ak" && WeaponAudioGunId("assets/Weapons/Remington870/Remington870.fpsanim") == "870");
    wa.Stop();
}

void TestProfileComponentAndJson() {
    WeaponAudioComponent c;
    c.Gun = "ak";
    c.Volume = 0.5f;
    c.CloseFullDistance = 30.0f;
    c.CloseZeroDistance = 80.0f;
    c.FarMinWeight = 0.2f;
    c.TailMaxVoices = 2;
    c.TailFadeTime = 0.4f;
    c.PlayerGain = 0.6f;
    WeaponAudioProfile p = WeaponAudioProfile::Default("ak");
    p.ApplyComponent(c);
    CHECK(p.Volume == 0.5f && p.PlayerGain == 0.6f && p.Close.Curve.NearDistance == 30.0f && p.Close.Curve.FarDistance == 80.0f);
    CHECK(p.Far.Curve.NearWeight == 0.2f && p.Far.Curve.NearDistance == 30.0f);
    CHECK(p.Tail.Set.MaxVoices == 2 && p.Tail.Set.StealFadeTime == 0.4f);
    p.ApplyJson(R"({"layers":{"close":{"volume":0.4,"curve":{"farDistance":60}}},"events":{"mag_out":{"files":["a.wav","b.wav"],"maxVoices":2,"volume":0.8}}})");
    CHECK(p.Close.Set.Volume == 0.4f && p.Close.Curve.FarDistance == 60.0f && p.Close.Curve.NearDistance == 30.0f);
    CHECK(p.Close.Set.Key == "snd.ak.fire_close"); // keys stay put
    CHECK(p.Events.count("mag_out") == 1 && p.Events["mag_out"].Files.size() == 2 && p.Events["mag_out"].MaxVoices == 2);
    CHECK(p.Events["mag_out"].Key == "snd.ak.mag_out");
    // The profile writes itself out and reads back the same.
    WeaponAudioProfile q = WeaponAudioProfile::Default("ak");
    q.ApplyJson(p.ToJson());
    CHECK(q.Close.Curve.FarDistance == 60.0f && q.Events["mag_out"].Files == p.Events["mag_out"].Files && q.Tail.Set.MaxVoices == 2);
    // The reflected components are registered (inspector + scene serialization).
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    bool weapon = false, foley = false;
    for (const auto& rc : ComponentRegistry::All()) {
        weapon |= std::string(rc.Meta.Name) == "Weapon Audio";
        foley |= std::string(rc.Meta.Name) == "Foley Audio";
    }
    CHECK(weapon && foley);
}

void TestAudioManifestGroupsVariants() {
    const char* text = R"({"files":[
        {"file":"Weapons/AKS74U/mag_out_2.wav","key":"snd.ak.mag_out","category":"weapon","layer":""},
        {"file":"Weapons/AKS74U/mag_out_1.wav","key":"snd.ak.mag_out","category":"weapon","layer":""},
        {"file":"Weapons/AKS74U/shot_close_1.wav","key":"snd.ak.shot","category":"weapon","layer":"close"},
        {"file":"assets/Audio/Foley/concrete/walk_3.wav","key":"snd.foley.concrete.walk","category":"foley","layer":""},
        {"file":"Weapons/AKS74U/mag_out_3.wav","key":"snd.ak.mag_out","category":"weapon","layer":""}]})";
    SoundManifest m;
    std::string err;
    CHECK(SoundManifest::FromJson(text, m, &err));
    CHECK(m.Entries.size() == 5);
    const std::vector<std::string> mag = m.FilesFor("snd.ak.mag_out");
    CHECK(mag.size() == 3 && mag[0] == "assets/Audio/Weapons/AKS74U/mag_out_1.wav" && mag[2] == "assets/Audio/Weapons/AKS74U/mag_out_3.wav");
    CHECK(m.FilesFor("snd.foley.concrete.walk").size() == 1 && m.FilesFor("snd.foley.concrete.walk")[0] == "assets/Audio/Foley/concrete/walk_3.wav");
    CHECK(m.FilesFor("snd.nope").empty());
    CHECK(m.Keys().size() == 3);
    // The object form (file -> fields) and a bare list read the same.
    SoundManifest obj, arr;
    CHECK(SoundManifest::FromJson(R"({"Weapons/x_1.wav":{"key":"snd.k.a"},"Weapons/x_2.wav":{"key":"snd.k.a"}})", obj));
    CHECK(obj.FilesFor("snd.k.a").size() == 2);
    CHECK(SoundManifest::FromJson(R"([{"file":"a_1.wav","key":"snd.k.b"}])", arr) && arr.FilesFor("snd.k.b").size() == 1);
    CHECK(!SoundManifest::FromJson("{not json", obj));

    // The contract's file layout as the fallback: variants are <element>_<n>.wav in the gun's / category's folder.
    CHECK(SoundKeyFilePrefix("snd.ak.mag_out") == "assets/Audio/Weapons/AKS74U/mag_out_");
    CHECK(SoundKeyFilePrefix("snd.870.pump_back") == "assets/Audio/Weapons/Remington870/pump_back_");
    CHECK(SoundKeyFilePrefix("snd.foley.concrete.walk") == "assets/Audio/Foley/concrete/walk_");
    CHECK(SoundKeyFilePrefix("Shot").empty() && SoundKeyFilePrefix("snd.ak").empty());
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "tartarus_audio_layout_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets/Audio/Weapons/AKS74U", ec);
    fs::create_directories(root / "assets/Audio/Foley/concrete", ec);
    for (const char* f : {"assets/Audio/Weapons/AKS74U/mag_out_2.wav", "assets/Audio/Weapons/AKS74U/mag_out_1.wav",
                          "assets/Audio/Weapons/AKS74U/mag_out_long_1.wav", "assets/Audio/Weapons/AKS74U/mag_in_1.wav",
                          "assets/Audio/Foley/concrete/walk_1.wav"})
        std::ofstream(root / f).put('x');
    const std::vector<std::string> files = SoundFilesByLayout(root.string(), "snd.ak.mag_out");
    CHECK(files.size() == 2 && files[0] == "assets/Audio/Weapons/AKS74U/mag_out_1.wav" && files[1] == "assets/Audio/Weapons/AKS74U/mag_out_2.wav");
    CHECK(SoundFilesByLayout(root.string(), "snd.foley.concrete.walk").size() == 1);
    CHECK(SoundFilesByLayout(root.string(), "snd.ak.missing").empty());
    // A set with no files of its own is filled from the layout, and variants are what plays.
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest(root.string(), &be);
    WeaponAudioProfile* ak = wa.Profile("ak");
    SoundSet* s = wa.EventSet(*ak, "mag_out");
    CHECK(s->Files.size() == 2);
    wa.PlayEvent("snd.ak.mag_out", "", glm::vec3(0.0f), true);
    CHECK(be.Voices.size() == 1 && be.Voices[0].Voice.File.find("mag_out_") != std::string::npos);
    wa.Stop();
    fs::remove_all(root, ec);
}

void TestFoleyFootstepCadence() {
    World world;
    FakeBackend be;
    WeaponAudio::Get().StartForTest("", &be);
    FoleyAudio& fo = FoleyAudio::Get();
    FoleyAudioComponent t; // defaults
    auto stepsAt = [&](float speed, bool sprint, bool crouch, float seconds) {
        fo.StartForTest(t);
        WeaponAudio::Get().ClearHistory();
        FoleyPlayerInput in;
        in.Velocity = glm::vec3(speed, 0.0f, 0.0f);
        in.Sprinting = sprint;
        in.Crouched = crouch;
        const float dt = 1.0f / 60.0f;
        for (float x = 0.0f; x < seconds; x += dt) fo.UpdatePlayer(world, dt, in);
        return fo.StepsPlayed();
    };
    // One footfall every half stride (0.8 m walking at the default 1.6 m stride): the cadence follows the speed.
    const int walk2 = stepsAt(2.0f, false, false, 10.0f);
    const int walk4 = stepsAt(4.0f, false, false, 10.0f);
    CHECK(std::abs(walk2 - 25) <= 1);
    CHECK(std::abs(walk4 - 50) <= 1);
    const int run6 = stepsAt(6.0f, true, false, 10.0f);
    CHECK(std::abs(run6 - 55) <= 1); // 60 m / 1.1 m
    const int crouch = stepsAt(1.5f, false, true, 10.0f);
    CHECK(std::abs(crouch - 23) <= 1); // 15 m / 0.64 m
    CHECK(stepsAt(0.3f, false, false, 10.0f) == 0); // below the minimum speed: standing still makes no steps
    // The set follows the gait, on the default surface (no physics world here).
    stepsAt(2.0f, false, false, 1.0f);
    CHECK(!WeaponAudio::Get().History().empty() && WeaponAudio::Get().History().front().Key == "snd.foley.step_concrete.walk" && WeaponAudio::Get().History().front().At2D);
    stepsAt(6.0f, true, false, 1.0f);
    CHECK(WeaponAudio::Get().History().front().Key == "snd.foley.step_concrete.run");
    stepsAt(1.5f, false, true, 1.0f);
    CHECK(WeaponAudio::Get().History().front().Key == "snd.foley.step_concrete.crouch");
    // Slower strides (the scale tunes it) mean fewer steps.
    t.StepStrideScale = 2.0f;
    CHECK(std::abs(stepsAt(2.0f, false, false, 10.0f) - 12) <= 1);
    t = FoleyAudioComponent{};
    // Airborne: no steps. Disabled: none either.
    {
        fo.StartForTest(t);
        FoleyPlayerInput in;
        in.Velocity = glm::vec3(4.0f, -1.0f, 0.0f);
        in.Grounded = false;
        for (int i = 0; i < 120; ++i) fo.UpdatePlayer(world, 1.0f / 60.0f, in);
        CHECK(fo.StepsPlayed() == 0);
        t.Enabled = false;
        fo.StartForTest(t);
        in.Grounded = true;
        for (int i = 0; i < 120; ++i) fo.UpdatePlayer(world, 1.0f / 60.0f, in);
        CHECK(fo.StepsPlayed() == 0);
        t.Enabled = true;
    }
    // Soldiers: per-id strides on the same rule, 3D, never counted against each other.
    fo.StartForTest(t);
    WeaponAudio::Get().ClearHistory();
    for (int i = 0; i < 600; ++i) {
        fo.NpcWalk(world, 1, glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), false, 1.0f / 60.0f);
        fo.NpcWalk(world, 2, glm::vec3(0.0f), glm::vec3(4.0f, 0.0f, 0.0f), false, 1.0f / 60.0f);
    }
    int npcSteps = 0;
    bool all3D = true;
    for (const auto& e : WeaponAudio::Get().History()) {
        ++npcSteps;
        all3D &= !e.At2D;
    }
    CHECK(std::abs(npcSteps - 75) <= 2 && all3D); // 25 + 50
    WeaponAudio::Get().Stop();
}

void TestFoleyRules() {
    FoleyAudioComponent t;
    // Surfaces by the ground's material / tag / name, in table order, case-insensitive; the default otherwise.
    CHECK(FoleyAudio::SurfaceFromName(t.SurfaceTable, "Assets/Materials/WoodPlank.physicmaterial", "concrete") == "wood");
    CHECK(FoleyAudio::SurfaceFromName(t.SurfaceTable, "Metal Grate 02", "concrete") == "metal");
    CHECK(FoleyAudio::SurfaceFromName(t.SurfaceTable, "Red CARPET", "concrete") == "carpet");
    CHECK(FoleyAudio::SurfaceFromName(t.SurfaceTable, "Cube (12)", "concrete") == "concrete");
    CHECK(FoleyAudio::SurfaceFromName(t.SurfaceTable, "Cube (12)", "dirt") == "dirt");
    CHECK(FoleyAudio::SurfaceFromName("a=x;b=y,z", "ZZ", "q") == "b" && FoleyAudio::SurfaceFromName("", "x", "q") == "q");
    // Landings scale with the fall: silent below the minimum, ramping to the full volume.
    CHECK(FoleyAudio::LandGain(t, 1.0f) == 0.0f && FoleyAudio::LandGain(t, t.LandMinSpeed - 0.01f) == 0.0f);
    const float soft = FoleyAudio::LandGain(t, 4.0f), hard = FoleyAudio::LandGain(t, 8.0f), huge = FoleyAudio::LandGain(t, 30.0f);
    CHECK(soft > 0.0f && soft < hard && hard < huge + 1e-6f && std::fabs(huge - t.LandVolume) < 1e-5f);
    // The cloth loop rides the speed: silent below its minimum, full at the run speed, none in the air.
    CHECK(FoleyAudio::ClothLoopGain(t, 1.0f, true) == 0.0f && FoleyAudio::ClothLoopGain(t, 6.0f, false) == 0.0f);
    const float mid = FoleyAudio::ClothLoopGain(t, 0.5f * (t.ClothLoopMinSpeed + t.RunSpeed), true);
    CHECK(std::fabs(mid - 0.5f * t.ClothLoopVolume) < 1e-5f && std::fabs(FoleyAudio::ClothLoopGain(t, 10.0f, true) - t.ClothLoopVolume) < 1e-5f);
    // Stride: half the (walk .. sprint) stride, scaled; crouching shortens it.
    FoleyPlayerInput in;
    in.WalkStride = 2.0f;
    in.SprintStride = 3.0f;
    CHECK(std::fabs(FoleyAudio::StepDistance(t, in) - 1.0f) < 1e-5f);
    in.Sprinting = true;
    CHECK(std::fabs(FoleyAudio::StepDistance(t, in) - 1.5f) < 1e-5f);
    in.Sprinting = false;
    in.Crouched = true;
    CHECK(std::fabs(FoleyAudio::StepDistance(t, in) - 0.8f) < 1e-5f);
    FoleyStepper st;
    CHECK(st.Advance(0.5f, 1.0f) == 0 && st.Advance(0.6f, 1.0f) == 1 && std::fabs(st.Accum - 0.1f) < 1e-5f && st.Advance(2.45f, 1.0f) == 2);
}

void TestFoleyLandingAndJump() {
    World world;
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    FoleyAudio& fo = FoleyAudio::Get();
    FoleyAudioComponent t;
    auto land = [&](float fallSpeed) {
        fo.StartForTest(t);
        wa.ClearHistory();
        FoleyPlayerInput in;
        in.Grounded = false;
        in.Velocity = glm::vec3(0.0f, -fallSpeed, 0.0f);
        for (int i = 0; i < 10; ++i) fo.UpdatePlayer(world, 1.0f / 60.0f, in);
        in.Grounded = true;
        in.Velocity = glm::vec3(0.0f);
        fo.UpdatePlayer(world, 1.0f / 60.0f, in);
        return (int)std::count_if(wa.History().begin(), wa.History().end(), [](const WeaponAudio::Emitted& e) { return e.Key == "snd.foley.move.land_heavy"; });
    };
    CHECK(land(1.0f) == 0); // a hop: silent
    CHECK(land(9.0f) == 1); // a hard landing: once
    fo.StartForTest(t);
    wa.ClearHistory();
    FoleyPlayerInput j;
    j.Jumped = true;
    fo.UpdatePlayer(world, 1.0f / 60.0f, j);
    CHECK(wa.History().size() == 1 && wa.History()[0].Key == "snd.foley.move.jump");
    wa.Stop();
}

void TestFullAutoDecimationAndHeadroom() {
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    WeaponAudioProfile* p = wa.Profile("ak");
    for (WeaponAudioProfile::Layer* l : {&p->Close, &p->Mech, &p->Sub, &p->Tail, &p->Far}) {
        l->Set.Files = {std::string("f_") + l->Set.Key + ".wav"};
        l->Set.FilePeakDb = {-9.0f}; // each layer peaks at -9 dBTP
        l->Set.StealFadeTime = 0.0f;
        l->Set.MaxVoices = 64;
    }
    auto count = [&](const WeaponAudioProfile::Layer& l) { return be.Started(l.Set.Files[0]); };
    // Policy: close / mech / sub every shot, tail every 2nd, far every 3rd of a burst; the first shot of a burst plays all.
    CHECK(p->Tail.Every == 2 && p->Far.Every == 3 && p->Close.Every == 1);
    const float dt = 60.0f / 650.0f; // 650 rpm
    for (int i = 0; i < 30; ++i) {
        wa.Shot("ak", glm::vec3(60.0f, 0.0f, 0.0f), false); // far away: tail and far audible, the crack is out of range
        wa.Update(dt);
    }
    CHECK(count(p->Tail) == 15);
    CHECK(count(p->Far) == 10);
    be.Voices.clear();
    wa.Update(2.0f);
    for (int i = 0; i < 30; ++i) {
        wa.Shot("ak", glm::vec3(0.0f), true);
        wa.Update(dt);
    }
    CHECK(count(p->Close) == 30 && count(p->Mech) == 30 && count(p->Sub) == 30);
    CHECK(count(p->Tail) == 15 && count(p->Far) == 0); // the shooter doesn't hear the distant layer
    // A new burst starts on a full report again.
    be.Voices.clear();
    wa.Update(1.0f);
    wa.Shot("ak", glm::vec3(0.0f), true);
    CHECK(count(p->Tail) == 1 && count(p->Close) == 1);
    // The rate is the profile's: every 1 plays every shot.
    p->Tail.Every = 1;
    be.Voices.clear();
    wa.Update(1.0f);
    for (int i = 0; i < 6; ++i) {
        wa.Shot("ak", glm::vec3(0.0f), true);
        wa.Update(dt);
    }
    CHECK(count(p->Tail) == 6);

    // Headroom: ten shots of three layers peaking at -4 dBTP each, overlapping inside the limiter window.
    auto peakOfBurst = [&](bool limiter) {
        FakeBackend fb;
        wa.StartForTest("", &fb);
        SoundPlayer::Limiter lim;
        lim.Enabled = limiter;
        lim.MinGain = 0.02f; // (the ceiling, not the audibility floor, is measured)
        wa.Player().SetLimiter(lim);
        WeaponAudioProfile* q = wa.Profile("ak");
        q->PlayerGain = 1.0f;
        for (WeaponAudioProfile::Layer* l : {&q->Close, &q->Mech, &q->Sub}) {
            l->Set.Files = {"x.wav"};
            l->Set.FilePeakDb = {-4.0f};
            l->Set.VolumeJitterDb = 0.0f;
        }
        q->Tail.Set.Files.clear();
        q->Far.Set.Files.clear();
        std::vector<std::pair<double, float>> starts; // time, peak amplitude
        const float dt2 = 60.0f / 800.0f;
        double now = 0.0;
        float worst = 0.0f;
        for (int i = 0; i < 10; ++i) {
            const size_t before = fb.Voices.size();
            wa.Shot("ak", glm::vec3(0.0f), true);
            for (size_t v = before; v < fb.Voices.size(); ++v) starts.push_back({now, fb.Voices[v].Volume * std::pow(10.0f, -4.0f / 20.0f)});
            // The summed peaks of every transient still inside the window.
            float sum = 0.0f;
            for (const auto& [t, a] : starts)
                if (now - t < lim.Window) sum += a * (1.0f - (float)(now - t) / lim.Window);
            worst = std::max(worst, sum);
            wa.Update(dt2);
            now += dt2;
        }
        wa.Stop(); // (before `fb` goes: the player still holds its backend)
        return worst;
    };
    const float unlimited = peakOfBurst(false), limited = peakOfBurst(true);
    CHECK(unlimited > 1.5f); // without it the burst sums well past 0 dBFS
    CHECK(limited < 1.0f);   // with it, under full scale ...
    CHECK(limited > 0.3f);   // ... and not silenced
    // The ceiling is a setting: lower it and the burst stays lower.
    {
        FakeBackend fb;
        SoundPlayer pl(&fb);
        SoundPlayer::Limiter lim;
        lim.CeilingDb = -12.0f;
        lim.MinGain = 0.0f;
        pl.SetLimiter(lim);
        SoundSet s = MakeSet("t.lim", {"a.wav"}, 0);
        s.FilePeakDb = {0.0f};
        pl.Play(s, {});
        pl.Play(s, {});
        pl.Play(s, {});
        CHECK(pl.LimiterLoad() < std::pow(10.0f, -12.0f / 20.0f) * 1.3f);
    }
    wa.Stop();
}

void TestAnchoredEventsLandOnTheContact() {
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    WeaponAudioProfile* ak = wa.Profile("ak");
    // Two takes of one sound: the contact transient is 20 ms into the first file and 100 ms into the second.
    SoundSet s = MakeSet("snd.ak.bolt_back", {"a_20.wav", "b_100.wav"}, 8);
    s.NoImmediateRepeat = false;
    s.FileAnchorMs = {20.0f, 100.0f};
    ak->Events["bolt_back"] = s;
    float lead = 0.0f;
    std::string gun, el;
    CHECK(WeaponAudio::ParseKey("snd.ak.bolt_back@100", gun, el, &lead) && gun == "ak" && el == "bolt_back" && lead == 100.0f);
    CHECK(WeaponAudio::ParseKey("snd.ak.bolt_back", gun, el, &lead) && lead < 0.0f);
    // The event fires 100 ms ahead of the contact: whichever take plays, its transient lands on the contact.
    std::set<std::string> seen;
    for (int i = 0; i < 40 && seen.size() < 2; ++i) {
        be.Voices.clear();
        const double t0 = wa.Player().Now();
        wa.PlayEvent("snd.ak.bolt_back@100", "", glm::vec3(0.0f), true);
        double started = -1.0;
        float seek = 0.0f;
        std::string file;
        for (int f = 0; f < 200 && started < 0.0; ++f) {
            if (!be.Voices.empty()) {
                started = wa.Player().Now();
                seek = be.Voices[0].Voice.StartOffset;
                file = be.Voices[0].Voice.File;
            } else {
                wa.Update(0.001f);
            }
        }
        CHECK(started >= 0.0);
        const float anchor = file == "a_20.wav" ? 0.020f : 0.100f;
        // The transient's place on the timeline (start - seek + anchor) must be the contact, t0 + 100 ms.
        CHECK(std::fabs((started - seek + anchor) - (t0 + 0.100)) < 0.0025);
        seen.insert(file);
        wa.Update(0.3f);
    }
    CHECK(seen.size() == 2);
    // A clamped event (the contact is only 30 ms away, nearer than the second take's lead-in): it starts at once, 70 ms in.
    be.Voices.clear();
    wa.Update(0.5f);
    ak->Events["bolt_back"].Files = {"b_100.wav"};
    ak->Events["bolt_back"].FileAnchorMs = {100.0f};
    wa.PlayEvent("snd.ak.bolt_back@30", "", glm::vec3(0.0f), true);
    CHECK(be.Voices.size() == 1 && std::fabs(be.Voices[0].Voice.StartOffset - 0.070f) < 1e-4f);
    // A start that lands a frame late skips the overshoot, so the contact is still on time.
    be.Voices.clear();
    ak->Events["bolt_back"].FileAnchorMs = {0.0f};
    wa.PlayEvent("snd.ak.bolt_back@100", "", glm::vec3(0.0f), true);
    CHECK(be.Voices.empty());
    wa.Update(1.0f / 60.0f * 7.0f); // 117 ms: 17 ms past due
    CHECK(be.Voices.size() == 1 && std::fabs(be.Voices[0].Voice.StartOffset - 0.0167f) < 0.002f);
    // Gear sounds the shared foley owns are its keys; the gun's own stay the gun's.
    be.Voices.clear();
    CHECK(wa.PlayEvent("ads_in", "ak", glm::vec3(0.0f), true));
    CHECK(wa.History().back().Key == "snd.foley.weapon.ads_in");
    CHECK(wa.PlayEvent("firemode", "ak", glm::vec3(0.0f), true) && wa.History().back().Key == "snd.ak.firemode");
    wa.Stop();
}

} // namespace


// --- environment tails: probe, classifier, zones ----------------------------------------------------------------------

// A box room around the origin as the physics world would answer: the ray exits through the first face it crosses, which
// is a wall (hit) only when that face is solid; an open face (no ceiling, a doorway) is a miss.
struct BoxRoom {
    glm::vec3 Min, Max;
    bool Solid[6] = {true, true, true, true, true, true}; // -x +x -y +y -z +z
    EnvironmentProbe::RayFn Fn(int* casts = nullptr) const {
        const BoxRoom room = *this;
        return [room, casts](const glm::vec3& o, const glm::vec3& d, float maxD, float& hit) {
            if (casts) ++*casts;
            float tBest = 1e30f;
            int face = -1;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < 1e-6f) continue;
                const bool pos = d[a] > 0.0f;
                const float t = ((pos ? room.Max[a] : room.Min[a]) - o[a]) / d[a];
                if (t > 0.0f && t < tBest) {
                    tBest = t;
                    face = a * 2 + (pos ? 1 : 0);
                }
            }
            if (face < 0 || !room.Solid[face] || tBest > maxD) return false;
            hit = tBest;
            return true;
        };
    }
};

EnvironmentReading ProbeRoom(const BoxRoom& room, const EnvironmentSettings& s = EnvironmentSettings{}) {
    EnvironmentProbe probe;
    probe.SetRayFn(room.Fn());
    return probe.Probe(glm::vec3(0.0f, 1.5f, 0.0f), s);
}

float WeightSum(const float w[kSpaceClassCount]) { return w[0] + w[1] + w[2] + w[3]; }

void TestEnvironmentClassifier() {
    const EnvironmentSettings s;
    // The ray set: one up, a diagonal ring, a horizontal ring; count follows the setting.
    for (int n : {6, 12, 24, 64}) {
        const std::vector<EnvironmentRay> rays = EnvironmentProbe::Rays(n);
        CHECK((int)rays.size() == n);
        CHECK(rays[0].Type == EnvironmentRay::Kind::Up);
        int h = 0;
        for (const EnvironmentRay& r : rays) {
            CHECK(std::fabs(glm::length(r.Dir) - 1.0f) < 1e-4f);
            h += r.Type == EnvironmentRay::Kind::Horizontal ? 1 : 0;
        }
        CHECK(h >= 3);
    }
    CHECK(EnvironmentProbe::Rays(1).size() == 6 && EnvironmentProbe::Rays(500).size() == 64); // clamped

    // A 4 x 3 x 5 m room: tight, covered -> indoor_small.
    const EnvironmentReading small = ProbeRoom({{-2.0f, 0.0f, -2.5f}, {2.0f, 3.0f, 2.5f}});
    CHECK(small.Valid && small.Dominant == SpaceClass::IndoorSmall && small.CeilingHit && small.Cover > 0.95f);
    CHECK(small.Weights[(int)SpaceClass::IndoorSmall] > 0.9f);
    // A 40 x 12 x 50 m hall: covered, far walls -> indoor_large.
    const EnvironmentReading large = ProbeRoom({{-20.0f, 0.0f, -25.0f}, {20.0f, 12.0f, 25.0f}});
    CHECK(large.Dominant == SpaceClass::IndoorLarge && large.MeanWallDistance > s.LargeRoomDistance);
    CHECK(large.Weights[(int)SpaceClass::IndoorLarge] > 0.9f);
    // A courtyard: walls 10 m around, open to the sky -> outdoor_urban.
    BoxRoom court{{-10.0f, -50.0f, -10.0f}, {10.0f, 50.0f, 10.0f}};
    court.Solid[3] = false; // no ceiling
    const EnvironmentReading urban = ProbeRoom(court);
    CHECK(urban.Dominant == SpaceClass::OutdoorUrban && !urban.CeilingHit && urban.Wall > 0.9f);
    CHECK(urban.Weights[(int)SpaceClass::OutdoorUrban] > 0.9f);
    // Open ground: nothing in reach -> outdoor_open.
    BoxRoom field{{-500.0f, -50.0f, -500.0f}, {500.0f, 500.0f, 500.0f}};
    const EnvironmentReading open = ProbeRoom(field);
    CHECK(open.Dominant == SpaceClass::OutdoorOpen && open.Enclosure == 0.0f && open.Weights[(int)SpaceClass::OutdoorOpen] > 0.99f);
    CHECK(std::fabs(open.MeanDistance - s.MaxDistance) < 1e-3f); // a miss counts as the max distance
    // The same hall under a smaller max distance: its far walls are out of reach, so the room reads as open.
    EnvironmentSettings shortSight = s;
    shortSight.MaxDistance = 6.0f;
    CHECK(ProbeRoom({{-20.0f, 0.0f, -25.0f}, {20.0f, 12.0f, 25.0f}}, shortSight).Dominant == SpaceClass::OutdoorOpen);
    // A tunable threshold moves the decision: the courtyard stops being urban when its walls count as too far away.
    EnvironmentSettings farWalls = s;
    farWalls.UrbanDistance = 5.0f;
    CHECK(ProbeRoom(court, farWalls).Dominant == SpaceClass::OutdoorOpen);
    for (const EnvironmentReading& r : {small, large, urban, open}) CHECK(std::fabs(WeightSum(r.Weights) - 1.0f) < 1e-4f);
}

void TestEnvironmentCrossfadeWeights() {
    EnvironmentSettings s;
    // Every feature sweep: the weights sum to 1, stay in [0,1] and move in small steps (no hard switch), through each threshold.
    auto sweep = [&](auto feature, float from, float to, int steps, float maxStep) {
        float prev[kSpaceClassCount] = {0, 0, 0, 0};
        float worst = 0.0f;
        for (int i = 0; i <= steps; ++i) {
            const float x = from + (to - from) * (float)i / (float)steps;
            float w[kSpaceClassCount];
            feature(x, w);
            CHECK(std::fabs(WeightSum(w) - 1.0f) < 1e-4f);
            for (int c = 0; c < kSpaceClassCount; ++c) {
                CHECK(w[c] >= -1e-6f && w[c] <= 1.0f + 1e-6f);
                if (i > 0) worst = std::max(worst, std::fabs(w[c] - prev[c]));
                prev[c] = w[c];
            }
        }
        CHECK(worst <= maxStep);
    };
    sweep([&](float x, float* w) { EnvironmentProbe::Weights(x, 0.0f, 3.0f, s, w); }, 0.0f, 1.0f, 1000, 0.01f);      // cover
    sweep([&](float x, float* w) { EnvironmentProbe::Weights(0.0f, x, 3.0f, s, w); }, 0.0f, 1.0f, 1000, 0.01f);      // wall
    sweep([&](float x, float* w) { EnvironmentProbe::Weights(1.0f, 0.0f, x, s, w); }, 0.0f, 30.0f, 3000, 0.01f);     // wall distance
    // At each threshold the two classes it separates are even.
    float w[kSpaceClassCount];
    EnvironmentProbe::Weights(s.IndoorCover, 0.0f, 3.0f, s, w);
    CHECK(std::fabs(w[(int)SpaceClass::IndoorSmall] - 0.5f) < 1e-4f && std::fabs(w[(int)SpaceClass::OutdoorOpen] - 0.5f) < 1e-4f);
    EnvironmentProbe::Weights(0.0f, s.UrbanWall, 3.0f, s, w);
    CHECK(std::fabs(w[(int)SpaceClass::OutdoorUrban] - 0.5f) < 1e-4f && std::fabs(w[(int)SpaceClass::OutdoorOpen] - 0.5f) < 1e-4f);
    EnvironmentProbe::Weights(1.0f, 0.0f, s.LargeRoomDistance, s, w);
    CHECK(std::fabs(w[(int)SpaceClass::IndoorSmall] - 0.5f) < 1e-4f && std::fabs(w[(int)SpaceClass::IndoorLarge] - 0.5f) < 1e-4f);
    // Just either side of a threshold the dominant class flips but only by a hair of weight.
    float a[kSpaceClassCount], b[kSpaceClassCount];
    EnvironmentProbe::Weights(s.IndoorCover - 0.001f, 0.0f, 3.0f, s, a);
    EnvironmentProbe::Weights(s.IndoorCover + 0.001f, 0.0f, 3.0f, s, b);
    CHECK(std::fabs(a[(int)SpaceClass::IndoorSmall] - b[(int)SpaceClass::IndoorSmall]) < 0.01f);
    // A zero blend width is a hard switch, still a valid mix.
    s.BlendFraction = 0.0f;
    EnvironmentProbe::Weights(s.IndoorCover - 0.01f, 0.0f, 3.0f, s, a);
    EnvironmentProbe::Weights(s.IndoorCover + 0.01f, 0.0f, 3.0f, s, b);
    CHECK(a[(int)SpaceClass::OutdoorOpen] == 1.0f && b[(int)SpaceClass::IndoorSmall] == 1.0f);
}

void TestEnvironmentRefreshThrottle() {
    EnvironmentSettings s; // 0.25 s, 1 m
    int casts = 0;
    EnvironmentProbe probe;
    probe.SetRayFn(BoxRoom{{-2.0f, 0.0f, -2.0f}, {2.0f, 3.0f, 2.0f}}.Fn(&casts));
    const int rays = s.RayCount;
    bool refreshed = false;
    glm::vec3 p(0.0f, 1.5f, 0.0f);
    probe.Query(7, p, 0.0, s, &refreshed);
    CHECK(refreshed && casts == rays);
    probe.Query(7, p, 0.10, s, &refreshed);
    CHECK(!refreshed && casts == rays); // cached
    probe.Query(7, p + glm::vec3(0.5f, 0.0f, 0.0f), 0.20, s, &refreshed);
    CHECK(!refreshed); // moved less than a metre, younger than 0.25 s
    probe.Query(7, p, 0.26, s, &refreshed);
    CHECK(refreshed && casts == 2 * rays); // aged out
    probe.Query(7, p + glm::vec3(1.5f, 0.0f, 0.0f), 0.30, s, &refreshed);
    CHECK(refreshed && casts == 3 * rays); // moved more than a metre
    CHECK(probe.GetStats().Refreshes == 3 && probe.GetStats().Rays == 3 * rays);
    // Ten seconds of a shooter firing at 600 rpm (10 shots a second): 100 shots, a refresh every 0.25 s - not per shot.
    EnvironmentProbe burst;
    int bc = 0;
    burst.SetRayFn(BoxRoom{{-2.0f, 0.0f, -2.0f}, {2.0f, 3.0f, 2.0f}}.Fn(&bc));
    for (int i = 0; i < 100; ++i) burst.Query(1, p, 0.1 * i, s);
    CHECK(burst.GetStats().Refreshes >= 32 && burst.GetStats().Refreshes <= 36); // one per 0.3 s (the first shot past 0.25 s), not 100
    // The cadence is a setting: no refresh interval re-probes only on movement, a long one holds the reading.
    s.RefreshInterval = 100.0f;
    EnvironmentProbe held;
    held.SetRayFn(BoxRoom{{-2.0f, 0.0f, -2.0f}, {2.0f, 3.0f, 2.0f}}.Fn());
    for (int i = 0; i < 100; ++i) held.Query(1, p, 0.1 * i, s);
    CHECK(held.GetStats().Refreshes == 1);
    // Unnamed shooters are told apart by position: two close shots are one shooter, a far one another.
    s = EnvironmentSettings{};
    EnvironmentProbe npcs;
    npcs.SetRayFn(BoxRoom{{-2.0f, 0.0f, -2.0f}, {2.0f, 3.0f, 2.0f}}.Fn());
    npcs.Query(0, glm::vec3(0.0f), 0.0, s);
    npcs.Query(0, glm::vec3(0.4f, 0.0f, 0.0f), 0.05, s);
    CHECK(npcs.Shooters() == 1);
    npcs.Query(0, glm::vec3(30.0f, 0.0f, 0.0f), 0.1, s);
    CHECK(npcs.Shooters() == 2);
    // The number of rays and their reach come from the settings.
    int c2 = 0;
    EnvironmentProbe custom;
    custom.SetRayFn(BoxRoom{{-2.0f, 0.0f, -2.0f}, {2.0f, 3.0f, 2.0f}}.Fn(&c2));
    s.RayCount = 20;
    custom.Probe(p, s);
    CHECK(c2 == 20);
}

// A tail layer playing from fake files: key -> the file its set plays.
void GiveTailFiles(WeaponAudioProfile* p, bool generic, std::initializer_list<SpaceClass> classes) {
    if (generic) p->Tail.Set.Files = {"tail_generic.wav"};
    for (SpaceClass c : classes) p->TailClass[(int)c].Set.Files = {std::string("tail_") + SpaceClassName(c) + ".wav"};
    p->Tail.Set.FilePeakDb = {-30.0f}; // quiet files: the bus limiter does not duck these test voices
    for (auto& tc : p->TailClass) tc.Set.FilePeakDb = {-30.0f};
    for (WeaponAudioProfile::Layer* l : {&p->Tail, &p->TailClass[0], &p->TailClass[1], &p->TailClass[2], &p->TailClass[3]}) {
        l->Set.StealFadeTime = 0.0f;
        l->Set.MaxVoices = 64;
    }
    p->Tail.Every = 1;
}

void TestEnvironmentTailSelectionAndFallback() {
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    WeaponAudioProfile* p = wa.Profile("ak");
    for (WeaponAudioProfile::Layer* l : {&p->Close, &p->Mech, &p->Sub, &p->Far}) l->Set.Files.clear();
    p->PlayerGain = 1.0f;
    p->Tail.Set.VolumeJitterDb = 0.0f;
    for (auto& tc : p->TailClass) tc.Set.VolumeJitterDb = 0.0f;
    GiveTailFiles(p, true, {SpaceClass::OutdoorOpen, SpaceClass::IndoorSmall}); // urban and large have no recordings yet
    auto fire = [&](const BoxRoom& room, double dt = 1.0) {
        wa.Probe().Clear();
        wa.Probe().SetRayFn(room.Fn());
        be.Voices.clear();
        wa.Update((float)dt);
        wa.Shot("ak", glm::vec3(0.0f, 1.5f, 0.0f), true);
    };
    BoxRoom small{{-2.0f, 0.0f, -2.5f}, {2.0f, 3.0f, 2.5f}};
    BoxRoom hall{{-20.0f, 0.0f, -25.0f}, {20.0f, 12.0f, 25.0f}};
    BoxRoom field{{-500.0f, -50.0f, -500.0f}, {500.0f, 500.0f, 500.0f}};
    fire(small);
    CHECK(be.Started("tail_indoor_small.wav") == 1 && be.Started("tail_generic.wav") == 0 && be.Voices.size() == 1);
    CHECK(wa.LastSpace().Valid && wa.LastSpace().Probed && wa.LastSpace().Reading.Dominant == SpaceClass::IndoorSmall);
    fire(field);
    CHECK(be.Started("tail_outdoor_open.wav") == 1 && be.Started("tail_generic.wav") == 0);
    // A space with no files falls back to the generic fire_tail.
    fire(hall);
    CHECK(be.Started("tail_generic.wav") == 1 && be.Started("tail_indoor_large.wav") == 0 && be.Voices.size() == 1);
    // The logical emission stays the layer's key (the weapon test counts snd.ak.fire_tail) and says which space it was.
    const auto& hist = wa.History();
    CHECK(!hist.empty() && hist.back().Key == "snd.ak.fire_tail" && hist.back().Space == "generic");
    // Environment off: always the generic tail, and no probing at all.
    p->Env.Enabled = false;
    int casts = 0;
    wa.Probe().SetRayFn(small.Fn(&casts));
    be.Voices.clear();
    wa.Update(1.0f);
    wa.Shot("ak", glm::vec3(0.0f, 1.5f, 0.0f), true);
    CHECK(be.Started("tail_generic.wav") == 1 && casts == 0 && !wa.LastSpace().Valid);
    p->Env.Enabled = true;
    // The generic set empty and the class's empty too: nothing to play, nothing crashes.
    p->Tail.Set.Files.clear();
    fire(hall);
    CHECK(be.Voices.empty());
    // Per-space tail gain scales the class tail.
    GiveTailFiles(p, true, {SpaceClass::IndoorSmall});
    p->Env.TailGain[(int)SpaceClass::IndoorSmall] = 0.5f;
    fire(small);
    CHECK(be.Voices.size() == 1 && std::fabs(be.Voices[0].Voice.Volume - 0.5f * p->Tail.Set.Volume) < 0.02f);
    // The probe is not asked per shot: a burst at 600 rpm probes about every 0.25 s.
    p->Env.TailGain[(int)SpaceClass::IndoorSmall] = 1.0f;
    int burstCasts = 0;
    wa.Probe().Clear();
    wa.Probe().SetRayFn(small.Fn(&burstCasts));
    wa.Update(5.0f);
    for (int i = 0; i < 40; ++i) { // 4 s at 10 shots / s
        wa.Shot("ak", glm::vec3(0.0f, 1.5f, 0.0f), true);
        wa.Update(0.1f);
    }
    CHECK(burstCasts >= 12 * p->Env.RayCount && burstCasts <= 16 * p->Env.RayCount); // ~ 14 probes (one per 0.3 s), not 40
    wa.Stop();
}

void TestReverbZoneContainmentPriorityAndBlend() {
    // Containment: a box turned 90 degrees (extents 8 x 2 x 3 -> 3 deep in world x, 8 in world z).
    ReverbZoneVolume box;
    box.Center = glm::vec3(10.0f, 0.0f, 0.0f);
    box.Shape = 0;
    box.Extents = glm::vec3(8.0f, 2.0f, 3.0f);
    box.ToLocal = glm::transpose(glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))));
    box.FadeDistance = 0.0f;
    box.Class = SpaceClass::IndoorLarge;
    CHECK(box.Weight(glm::vec3(10.0f, 0.0f, 7.5f)) == 1.0f);  // along the long (rotated) axis
    CHECK(box.Weight(glm::vec3(10.0f, 0.0f, -7.5f)) == 1.0f);
    CHECK(box.Weight(glm::vec3(16.0f, 0.0f, 0.0f)) == 0.0f);  // beyond the short side
    CHECK(box.Weight(glm::vec3(12.5f, 0.0f, 0.0f)) == 1.0f);
    CHECK(box.Weight(glm::vec3(10.0f, 2.5f, 0.0f)) == 0.0f);  // above
    ReverbZoneVolume ball;
    ball.Center = glm::vec3(0.0f);
    ball.Shape = 1;
    ball.Radius = 5.0f;
    ball.FadeDistance = 0.0f;
    CHECK(ball.Weight(glm::vec3(3.0f, 3.0f, 0.0f)) == 1.0f && ball.Weight(glm::vec3(3.6f, 3.6f, 0.0f)) == 0.0f);
    // Edge blend: weight 0 at the surface, smoothstep to 1 FadeDistance inside (half way = 0.5).
    ball.FadeDistance = 4.0f;
    CHECK(ball.Weight(glm::vec3(5.0f, 0.0f, 0.0f)) == 0.0f);
    CHECK(std::fabs(ball.Weight(glm::vec3(3.0f, 0.0f, 0.0f)) - 0.5f) < 1e-4f);
    CHECK(ball.Weight(glm::vec3(1.0f, 0.0f, 0.0f)) == 1.0f && ball.Weight(glm::vec3(0.0f)) == 1.0f);
    float prev = 0.0f;
    for (int i = 0; i <= 500; ++i) { // continuous and monotonic across the fade
        const float w = ball.Weight(glm::vec3(5.0f - 0.01f * (float)i, 0.0f, 0.0f));
        CHECK(w >= prev - 1e-6f && w - prev < 0.02f);
        prev = w;
    }

    // Layering: indoor_small (priority 1) inside a hall (priority 0) inside nothing; what no zone claims is the probe's.
    ReverbZoneVolume hall = ball, room = ball;
    hall.Radius = 20.0f;
    hall.FadeDistance = 4.0f;
    hall.Class = SpaceClass::IndoorLarge;
    hall.Priority = 0;
    hall.TailGain = 2.0f;
    room.Radius = 5.0f;
    room.FadeDistance = 2.0f;
    room.Class = SpaceClass::IndoorSmall;
    room.Priority = 1;
    ReverbZones zones;
    zones.Set({hall, room}); // given low-priority first: Set sorts
    CHECK(zones.Zones().size() == 2 && zones.Zones()[0].Priority == 1);
    ReverbZoneMix deep = zones.Mix(glm::vec3(0.0f));
    CHECK(deep.Weights[(int)SpaceClass::IndoorSmall] == 1.0f && deep.Weights[(int)SpaceClass::IndoorLarge] == 0.0f && deep.ProbeShare == 0.0f); // priority wins
    ReverbZoneMix mid = zones.Mix(glm::vec3(4.0f, 0.0f, 0.0f)); // on the room's fade, deep in the hall
    CHECK(mid.Weights[(int)SpaceClass::IndoorSmall] > 0.05f && mid.Weights[(int)SpaceClass::IndoorSmall] < 0.95f);
    CHECK(mid.Weights[(int)SpaceClass::IndoorLarge] > 0.05f && mid.ProbeShare == 0.0f);
    CHECK(std::fabs(WeightSum(mid.Weights) + mid.ProbeShare - 1.0f) < 1e-5f);
    CHECK(mid.Gain > 1.0f && mid.Gain < 2.0f && mid.Zones == 2); // the hall's gain is only partly in
    const ReverbZoneMix edge = zones.Mix(glm::vec3(21.0f, 0.0f, 0.0f)); // outside both
    CHECK(edge.ProbeShare == 1.0f && edge.Zones == 0 && edge.Gain == 1.0f && WeightSum(edge.Weights) == 0.0f);
    const ReverbZoneMix rim = zones.Mix(glm::vec3(18.0f, 0.0f, 0.0f)); // on the hall's fade: part zone, part probe
    CHECK(rim.ProbeShare > 0.05f && rim.ProbeShare < 0.95f && std::fabs(WeightSum(rim.Weights) + rim.ProbeShare - 1.0f) < 1e-5f);
    // Same priority: the order is by entity id, deterministic.
    ReverbZoneVolume a = ball, b = ball;
    a.Entity = 5; a.Class = SpaceClass::OutdoorUrban;
    b.Entity = 3; b.Class = SpaceClass::OutdoorOpen;
    ReverbZones tie;
    tie.Set({a, b});
    CHECK(tie.Zones()[0].Entity == 3);

    // Zones come from the scene: Reverb Zone components placed by their entities, disabled ones left out.
    World world;
    auto mk = [&](const glm::vec3& at, int cls, bool enabled) {
        const entt::entity e = world.Registry.create();
        world.Registry.emplace<TransformComponent>(e).Position = at;
        ReverbZoneComponent& z = world.Registry.emplace<ReverbZoneComponent>(e);
        z.TailClass = cls;
        z.Enabled = enabled;
        z.FadeDistance = 0.0f;
        z.Extents = glm::vec3(2.0f);
        return e;
    };
    mk(glm::vec3(100.0f, 0.0f, 0.0f), 3, true);
    mk(glm::vec3(0.0f, 0.0f, 100.0f), 2, false);
    ReverbZones built;
    built.Build(world);
    CHECK(built.Zones().size() == 1 && built.Zones()[0].Class == SpaceClass::IndoorLarge);
    CHECK(built.Mix(glm::vec3(101.0f, 0.5f, -1.0f)).Weights[(int)SpaceClass::IndoorLarge] == 1.0f);
    CHECK(built.Mix(glm::vec3(0.0f, 0.0f, 100.0f)).ProbeShare == 1.0f);
    // The reverb values are stored: the class presets by default, the component's own with Custom.
    ReverbZoneComponent zc;
    zc.TailClass = 3;
    CHECK(zc.Resolved().DecayTime == ReverbPresetFor(3).DecayTime);
    zc.ReverbMode = 1;
    zc.Reverb.DecayTime = 7.0f;
    CHECK(zc.Resolved().DecayTime == 7.0f);
    CHECK(ReverbPresetFor(2).DecayTime < ReverbPresetFor(3).DecayTime && ReverbPresetFor(3).DecayTime < ReverbPresetFor(0).DecayTime);
    CHECK(ReverbPresetFor(2).RoomSize < ReverbPresetFor(3).RoomSize && ReverbPresetFor(2).PreDelayMs < ReverbPresetFor(3).PreDelayMs);
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    bool registered = false;
    for (const auto& rc : ComponentRegistry::All()) registered |= std::string(rc.Meta.Name) == "Reverb Zone";
    CHECK(registered);
}

void TestEnvironmentZonesBeatTheProbe() {
    FakeBackend be;
    WeaponAudio& wa = WeaponAudio::Get();
    wa.StartForTest("", &be);
    WeaponAudioProfile* p = wa.Profile("ak");
    for (WeaponAudioProfile::Layer* l : {&p->Close, &p->Mech, &p->Sub, &p->Far}) l->Set.Files.clear();
    p->PlayerGain = 1.0f;
    p->Tail.Set.VolumeJitterDb = 0.0f;
    for (auto& tc : p->TailClass) tc.Set.VolumeJitterDb = 0.0f;
    GiveTailFiles(p, true, {SpaceClass::OutdoorOpen, SpaceClass::OutdoorUrban, SpaceClass::IndoorSmall, SpaceClass::IndoorLarge});
    int casts = 0;
    BoxRoom field{{-500.0f, -50.0f, -500.0f}, {500.0f, 500.0f, 500.0f}};
    wa.Probe().SetRayFn(field.Fn(&casts)); // the probe says: open ground
    ReverbZoneVolume z;
    z.Center = glm::vec3(0.0f);
    z.Shape = 0;
    z.Extents = glm::vec3(10.0f, 5.0f, 10.0f);
    z.FadeDistance = 4.0f;
    z.Class = SpaceClass::IndoorLarge;
    wa.Zones().Set({z});
    // Deep inside the zone: its class, and the probe is never asked.
    wa.Update(1.0f);
    wa.Shot("ak", glm::vec3(0.0f, 0.0f, 0.0f), true);
    CHECK(be.Started("tail_indoor_large.wav") == 1 && be.Voices.size() == 1 && casts == 0 && !wa.LastSpace().Probed);
    // Outside every zone: the probe decides.
    be.Voices.clear();
    wa.Update(1.0f);
    wa.Shot("ak", glm::vec3(50.0f, 0.0f, 0.0f), true);
    CHECK(be.Started("tail_outdoor_open.wav") == 1 && be.Voices.size() == 1 && casts > 0 && wa.LastSpace().Probed);
    // On the zone's edge: the two classes crossfade (equal-power) and sum to the same loudness as a single class at the middle.
    be.Voices.clear();
    wa.Update(1.0f);
    wa.Shot("ak", glm::vec3(8.0f, 0.0f, 0.0f), true); // 2 m inside a 4 m fade: half the zone, half the probe
    CHECK(be.Started("tail_indoor_large.wav") == 1 && be.Started("tail_outdoor_open.wav") == 1 && be.Voices.size() == 2);
    const float gl = be.Voices[0].Voice.File == "tail_indoor_large.wav" ? be.Voices[0].Volume : be.Voices[1].Volume;
    const float go = be.Voices[0].Voice.File == "tail_outdoor_open.wav" ? be.Voices[0].Volume : be.Voices[1].Volume;
    CHECK(std::fabs(gl - go) < 0.05f && std::fabs(gl * gl + go * go - 1.0f) < 0.1f); // sqrt(0.5) each
    CHECK(wa.History().back().Space.find('+') != std::string::npos);
    wa.Stop();
}

void RegisterAudioTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"SoundSetRoundRobinNeverRepeats", TestSoundSetRoundRobinNeverRepeats});
    tests.push_back({"SoundSetStealOldest", TestSoundSetStealOldest});
    tests.push_back({"SoundSetJitterPitchAndRequest", TestSoundSetJitterPitchAndRequest});
    tests.push_back({"SoundSetJsonRoundTrip", TestSoundSetJsonRoundTrip});
    tests.push_back({"DistanceBlendWeights", TestDistanceBlendWeights});
    tests.push_back({"AutofireVoiceCap", TestAutofireVoiceCap});
    tests.push_back({"FullAutoDecimationAndHeadroom", TestFullAutoDecimationAndHeadroom});
    tests.push_back({"AnchoredEventsLandOnTheContact", TestAnchoredEventsLandOnTheContact});
    tests.push_back({"AnimEventPlaysItsSet", TestAnimEventPlaysItsSet});
    tests.push_back({"WeaponAudioProfileComponentAndJson", TestProfileComponentAndJson});
    tests.push_back({"AudioManifestGroupsVariants", TestAudioManifestGroupsVariants});
    tests.push_back({"FoleyFootstepCadence", TestFoleyFootstepCadence});
    tests.push_back({"FoleyRules", TestFoleyRules});
    tests.push_back({"FoleyLandingAndJump", TestFoleyLandingAndJump});
    tests.push_back({"EnvironmentClassifier", TestEnvironmentClassifier});
    tests.push_back({"EnvironmentCrossfadeWeights", TestEnvironmentCrossfadeWeights});
    tests.push_back({"EnvironmentRefreshThrottle", TestEnvironmentRefreshThrottle});
    tests.push_back({"EnvironmentTailSelectionAndFallback", TestEnvironmentTailSelectionAndFallback});
    tests.push_back({"ReverbZoneContainmentPriorityAndBlend", TestReverbZoneContainmentPriorityAndBlend});
    tests.push_back({"EnvironmentZonesBeatTheProbe", TestEnvironmentZonesBeatTheProbe});
}
