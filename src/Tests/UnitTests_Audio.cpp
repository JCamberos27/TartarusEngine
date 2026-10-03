#include "UnitTestSupport.h"

#include "../Game/Audio/FoleyAudio.h"
#include "../Game/Audio/WeaponAudio.h"
#include "../Game/ComponentRegistry.h"
#include "World.h"

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
    player.SetLog([&](double, const std::string& key, const std::string& file, int, const SoundPlayer::Request&, float, float) {
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
    CHECK(p.Close.Set.Key == "snd.ak.shot_close"); // keys stay put
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
    CHECK(!WeaponAudio::Get().History().empty() && WeaponAudio::Get().History().front().Key == "snd.foley.concrete.walk" && WeaponAudio::Get().History().front().At2D);
    stepsAt(6.0f, true, false, 1.0f);
    CHECK(WeaponAudio::Get().History().front().Key == "snd.foley.concrete.run");
    stepsAt(1.5f, false, true, 1.0f);
    CHECK(WeaponAudio::Get().History().front().Key == "snd.foley.concrete.crouch");
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
    CHECK(FoleyAudio::SurfaceFromName(t.SurfaceTable, "TERRAIN GRASS", "concrete") == "grass");
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
        return (int)std::count_if(wa.History().begin(), wa.History().end(), [](const WeaponAudio::Emitted& e) { return e.Key == "snd.foley.concrete.land"; });
    };
    CHECK(land(1.0f) == 0); // a hop: silent
    CHECK(land(9.0f) == 1); // a hard landing: once
    fo.StartForTest(t);
    wa.ClearHistory();
    FoleyPlayerInput j;
    j.Jumped = true;
    fo.UpdatePlayer(world, 1.0f / 60.0f, j);
    CHECK(wa.History().size() == 1 && wa.History()[0].Key == "snd.foley.concrete.jump");
    wa.Stop();
}

} // namespace

void RegisterAudioTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"SoundSetRoundRobinNeverRepeats", TestSoundSetRoundRobinNeverRepeats});
    tests.push_back({"SoundSetStealOldest", TestSoundSetStealOldest});
    tests.push_back({"SoundSetJitterPitchAndRequest", TestSoundSetJitterPitchAndRequest});
    tests.push_back({"SoundSetJsonRoundTrip", TestSoundSetJsonRoundTrip});
    tests.push_back({"DistanceBlendWeights", TestDistanceBlendWeights});
    tests.push_back({"AutofireVoiceCap", TestAutofireVoiceCap});
    tests.push_back({"AnimEventPlaysItsSet", TestAnimEventPlaysItsSet});
    tests.push_back({"WeaponAudioProfileComponentAndJson", TestProfileComponentAndJson});
    tests.push_back({"AudioManifestGroupsVariants", TestAudioManifestGroupsVariants});
    tests.push_back({"FoleyFootstepCadence", TestFoleyFootstepCadence});
    tests.push_back({"FoleyRules", TestFoleyRules});
    tests.push_back({"FoleyLandingAndJump", TestFoleyLandingAndJump});
}
