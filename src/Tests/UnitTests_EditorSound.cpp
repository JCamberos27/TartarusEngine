#include "UnitTestSupport.h"

#include "../Audio/AudioEngine.h"
#include "../Editor/EditorSound.h"
#include "../Core/AppAudioMuter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

// Unit tests for the editor's UI sounds (Editor/EditorSound): the scheduler's rules and the engine's separate editor output.

namespace {

using EditorSound::Event;
using EditorSound::Scheduler;
using EditorSound::Settings;
using EditorSound::Voice;

void WriteToneWav(const std::filesystem::path& path, int rate, float seconds, float amp) {
    const int frames = (int)(seconds * (float)rate);
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + (std::uint32_t)frames * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32((std::uint32_t)rate); u32((std::uint32_t)rate * 2); u16(2); u16(16);
    f.write("data", 4); u32((std::uint32_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        const auto s = (std::int16_t)std::lround(amp * std::sin(6.2831853f * 880.0f * (float)i / (float)rate) * 32767.0f);
        f.write(reinterpret_cast<const char*>(&s), 2);
    }
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void TestEditorSoundEventTable() {
    std::set<std::string> names;
    for (int i = 0; i < EditorSoundEvents::kEventCount; ++i) {
        const EditorSound::EventInfo& info = EditorSound::Info((Event)i);
        CHECK(info.Name && *info.Name);
        names.insert(info.Name);
        CHECK(info.CooldownMs > 0.0f && info.PitchJitter >= 0.0f && info.PitchJitter < 0.1f);
        // Notifications and Play mode carry meaning from no place on screen: never panned.
        if (info.Cat == EditorSound::Category::Notifications || info.Cat == EditorSound::Category::PlayMode) CHECK(info.Centred);
    }
    CHECK((int)names.size() == EditorSoundEvents::kEventCount);
    for (int c = 0; c < EditorSoundEvents::kCategoryCount; ++c) CHECK(*EditorSound::CategoryName((EditorSound::Category)c));
}

void TestEditorSoundSchedulerRules() {
    Settings st;
    Scheduler sch(7u);
    Voice v;
    // No files: silent.
    CHECK(!sch.Decide(Event::Click, 0.0, 0.5f, true, st, v));

    // Variants round-robin and never repeat back to back; pitch stays inside the jitter.
    sch.SetVariantCount(Event::Click, 3);
    int last = -1;
    std::set<int> seen;
    double t = 1000.0;
    for (int i = 0; i < 300; ++i, t += 500.0) {
        CHECK(sch.Decide(Event::Click, t, 0.5f, true, st, v));
        CHECK(v.Variant >= 0 && v.Variant < 3 && v.Variant != last);
        CHECK(std::fabs(v.Pitch - 1.0f) <= EditorSound::Info(Event::Click).PitchJitter + 1e-6f);
        CHECK(std::fabs(v.Gain - 1.0f) < 1e-4f); // spaced out: no repeat attenuation
        last = v.Variant;
        seen.insert(v.Variant);
    }
    CHECK(seen.size() == 3);

    // Cooldown: a retrigger sooner than it is dropped.
    const float cd = EditorSound::Info(Event::Click).CooldownMs;
    CHECK(!sch.Decide(Event::Click, t - 500.0 + cd * 0.5, 0.5f, true, st, v));

    // Rapid repeats: each one quieter, down to the floor; a pause resets it.
    sch.Reset();
    float prev = 2.0f;
    double now = 0.0;
    for (int i = 0; i < 8; ++i, now += cd + 1.0) {
        CHECK(sch.Decide(Event::Click, now, 0.5f, true, st, v));
        CHECK(v.Gain <= prev + 1e-6f);
        prev = v.Gain;
    }
    CHECK(std::fabs(20.0f * std::log10(prev) - Scheduler::kRepeatFloorDb) < 0.01f);
    CHECK(sch.Decide(Event::Click, now + 1000.0, 0.5f, true, st, v) && std::fabs(v.Gain - 1.0f) < 1e-4f);

    // Pan: the window's edges pan kPanWidth, the middle and "no position" are centred, notifications never move.
    sch.Reset();
    CHECK(sch.Decide(Event::Click, 0.0, 0.0f, true, st, v) && std::fabs(v.Pan + Scheduler::kPanWidth) < 1e-5f);
    CHECK(sch.Decide(Event::Click, 1000.0, 1.0f, true, st, v) && std::fabs(v.Pan - Scheduler::kPanWidth) < 1e-5f);
    CHECK(sch.Decide(Event::Click, 2000.0, -1.0f, true, st, v) && v.Pan == 0.0f);
    sch.SetVariantCount(Event::NotifyError, 1);
    CHECK(sch.Decide(Event::NotifyError, 0.0, 0.0f, true, st, v) && v.Pan == 0.0f && v.Variant == 0);

    // In the background only notifications and the "done" chimes play.
    sch.Reset();
    CHECK(!sch.Decide(Event::Click, 0.0, 0.5f, false, st, v));
    CHECK(sch.Decide(Event::NotifyError, 0.0, 0.5f, false, st, v));
    sch.SetVariantCount(Event::ShaderReload, 1);
    sch.SetVariantCount(Event::Blocked, 1);
    CHECK(sch.Decide(Event::ShaderReload, 0.0, 0.5f, false, st, v));
    CHECK(!sch.Decide(Event::Blocked, 0.0, 0.5f, false, st, v)); // the rest of System waits for focus
    // P7: a packaged build's result is a done chime too; its progress ticks, the digest and the field sounds wait for focus.
    for (Event e : {Event::PackageSuccess, Event::PackageFail, Event::BuildProgress, Event::Digest, Event::FieldCommit})
        sch.SetVariantCount(e, 1);
    CHECK(sch.Decide(Event::PackageSuccess, 0.0, 0.5f, false, st, v));
    CHECK(sch.Decide(Event::PackageFail, 0.0, 0.5f, false, st, v));
    CHECK(!sch.Decide(Event::BuildProgress, 0.0, 0.5f, false, st, v));
    CHECK(!sch.Decide(Event::Digest, 0.0, 0.5f, false, st, v));
    CHECK(!sch.Decide(Event::FieldCommit, 0.0, 0.5f, false, st, v));
    // The digest counts exactly the done chimes.
    CHECK(EditorSound::IsDoneChime(Event::AssetImport) && EditorSound::IsDoneChime(Event::PackageFail) &&
          EditorSound::IsDoneChime(Event::BuildSuccess) && EditorSound::IsDoneChime(Event::ShaderReload));
    CHECK(!EditorSound::IsDoneChime(Event::NotifyError) && !EditorSound::IsDoneChime(Event::Digest) &&
          !EditorSound::IsDoneChime(Event::BuildProgress) && !EditorSound::IsDoneChime(Event::Click));

    // The switches: all off, a category off, a category at 0, master at 0.
    sch.Reset();
    Settings off = st;
    off.Enabled = false;
    CHECK(!sch.Decide(Event::Click, 0.0, 0.5f, true, off, v));
    off = st;
    off.CategoryOn[(int)EditorSound::Category::Clicks] = false;
    CHECK(!sch.Decide(Event::Click, 0.0, 0.5f, true, off, v));
    CHECK(sch.Decide(Event::NotifyError, 0.0, 0.5f, true, off, v)); // other categories unaffected
    off = st;
    off.CategoryVolume[(int)EditorSound::Category::Clicks] = 0.5f;
    CHECK(sch.Decide(Event::Click, 0.0, 0.5f, true, off, v) && std::fabs(v.Gain - 0.5f) < 1e-4f);
    off.Master = 0.0f;
    CHECK(!sch.Decide(Event::Click, 5000.0, 0.5f, true, off, v));
}

void TestEditorSoundFindVariants() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tartarus_editor_sound_ut";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteToneWav(dir / "Click_1.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(dir / "Click_2.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(dir / "Click_4.wav", 48000, 0.05f, 0.1f); // after a gap: not a variant
    WriteToneWav(dir / "Save.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(dir / "Redo.wav", 48000, 0.05f, 0.1f);   // a single file wins over numbered ones
    WriteToneWav(dir / "Redo_1.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(dir / "pause.wav", 48000, 0.05f, 0.1f);  // the listing matches names the way the folder did: any case
    fs::create_directories(dir / "Undo.wav", ec);          // a folder named like a file isn't one
    const auto click = EditorSound::FindVariants(dir.string(), Event::Click);
    CHECK(click.size() == 2);
    CHECK(click.size() == 2 && fs::u8path(click[0]).filename() == "Click_1.wav" && fs::u8path(click[1]).filename() == "Click_2.wav");
    const auto save = EditorSound::FindVariants(dir.string(), Event::Save);
    CHECK(save.size() == 1 && fs::u8path(save[0]) == dir / "Save.wav");
    CHECK(EditorSound::FindVariants(dir.string(), Event::Redo).size() == 1);
    CHECK(EditorSound::FindVariants(dir.string(), Event::Pause).size() == 1);
    CHECK(EditorSound::FindVariants(dir.string(), Event::Undo).empty());
    CHECK(EditorSound::FindVariants("", Event::Click).empty());
    CHECK(EditorSound::FindVariants((dir / "missing").string(), Event::Click).empty());
    fs::remove_all(dir, ec);
}

// The editor output is beside the game's mix: Mute and master don't reach it, the limiter's taps don't hear it, the voice cap holds.
void TestEditorSoundSeparateOutput() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tartarus_editor_out_ut";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const int rate = 48000;
    const std::string tone = (dir / "tick.wav").string();
    WriteToneWav(tone, rate, 0.2f, 0.5f);

    // TARTARUS_MUTE silences the endpoint, editor output included; an offline engine reaches no speaker, so unmute it here.
    std::string envMute;
#ifdef _WIN32
    char* m = nullptr;
    size_t len = 0;
    if (_dupenv_s(&m, &len, "TARTARUS_MUTE") == 0 && m) {
        envMute = m;
        std::free(m);
    }
#else
    if (const char* m = std::getenv("TARTARUS_MUTE")) envMute = m;
#endif
    SetEnv("TARTARUS_MUTE", "0");
    AudioEngine::InitOffline(rate);
    SetEnv("TARTARUS_MUTE", envMute.c_str());
    CHECK(AudioEngine::IsInitialized());
    if (!AudioEngine::IsInitialized()) return;
    AudioEngine::EnableTaps(true);
    AudioEngine::SetMuted(true);
    AudioEngine::SetMasterVolume(0.0f);
    AudioEngine::SetEditorVolume(1.0f);
    CHECK(AudioEngine::PlayEditor(tone, 1.0f, 1.0f, -1.0f));
    std::vector<float> out(2 * 480), master, pre, wet;
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < 10; ++i) {
        AudioEngine::RenderOffline(out.data(), 480);
        for (int k = 0; k < 480; ++k) {
            peakL = std::max(peakL, std::fabs(out[2 * k]));
            peakR = std::max(peakR, std::fabs(out[2 * k + 1]));
        }
    }
    AudioEngine::DrainTaps(master, pre, wet);
    float tapPeak = 0.0f;
    for (float s : master) tapPeak = std::max(tapPeak, std::fabs(s));
    CHECK(peakL > 0.1f);              // audible though the game is muted at master 0
    CHECK(peakL > peakR * 1.5f);      // panned left
    CHECK(tapPeak < 1e-6f);           // not in the game mix / capture
    CHECK(AudioEngine::EditorVoiceCount() == 1);
    AudioEngine::SetPaused(true);     // the game's pause doesn't stop it
    CHECK(AudioEngine::EditorVoiceCount() == 1);
    AudioEngine::SetPaused(false);

    // The cap: never more than kEditorVoices; the oldest goes.
    for (int i = 0; i < AudioEngine::kEditorVoices + 6; ++i) AudioEngine::PlayEditor(tone, 0.1f);
    CHECK(AudioEngine::EditorVoiceCount() == AudioEngine::kEditorVoices);
    // Reaped once they end.
    for (int i = 0; i < 40; ++i) { AudioEngine::RenderOffline(nullptr, 480); AudioEngine::Update(); }
    CHECK(AudioEngine::EditorVoiceCount() == 0);
    CHECK(!AudioEngine::PlayEditor((dir / "missing.wav").string(), 1.0f));

    AudioEngine::SetMuted(false);
    AudioEngine::SetMasterVolume(1.0f);
    AudioEngine::EnableTaps(false);
    AudioEngine::Shutdown();
    fs::remove_all(dir, ec);
}

// The Phase 2 state machine: slider grab / ticks / release, menus, hover and drag & drop from frame snapshots.
void TestEditorSoundUiObserver() {
    using EditorSound::UiFrame;
    using EditorSound::UiObserver;
    using EditorSound::UiSound;
    UiObserver o;
    std::vector<UiSound> out;
    auto step = [&](const UiFrame& f) { out.clear(); o.Step(f, out); };
    auto has = [&](Event e) { return std::any_of(out.begin(), out.end(), [e](const UiSound& u) { return u.E == e; }); };
    UiFrame f;
    step(f); // baseline
    CHECK(out.empty());

    // A button press: held, mouse down, never edited while down -> nothing here (the primitive clicks for itself).
    f.ActiveId = 7; f.MouseDown = true; step(f); CHECK(out.empty());
    f.ActiveId = 0; f.MouseDown = false; f.MouseReleased = true; f.Edited = true; step(f); CHECK(out.empty());
    f.MouseReleased = false; f.Edited = false;

    // A slider: grab on the first edit under a pressed mouse, a tick per kTickPx of travel, release when it lets go.
    f.ActiveId = 9; f.MouseDown = true; f.Edited = true; f.MouseDx = 1.0f; step(f);
    CHECK(has(Event::SliderGrab) && out.size() == 1);
    int ticks = 0;
    float lastPitch = 0.0f;
    for (int i = 0; i < 40; ++i) {
        f.MouseDx = 2.0f; step(f);
        for (const UiSound& u : out) if (u.E == Event::SliderTick) { ++ticks; CHECK(u.Pitch >= lastPitch - 1e-6f); lastPitch = u.Pitch; }
    }
    CHECK(ticks == (int)(2.0f * 40 / UiObserver::kTickPx) || ticks == (int)(2.0f * 40 / UiObserver::kTickPx) + 1);
    CHECK(lastPitch > 1.0f && lastPitch <= 1.0f + UiObserver::kPitchSwing + 1e-6f); // dragged right: pitch went up
    f.MouseDx = 0.0f; f.Edited = false; step(f); CHECK(out.empty()); // held still: quiet
    f.ActiveId = 0; f.MouseDown = false; step(f); CHECK(has(Event::SliderRelease));

    // Menus: open; a pick (released on an item in a menu) clicks; Escape / click-away closes.
    f.PopupDepth = 1; step(f); CHECK(has(Event::MenuOpen));
    f.HoverInMenu = f.HoverInPickMenu = true; f.HoveredId = 3; f.MouseDx = 1.0f; step(f); CHECK(has(Event::Hover));
    step(f); CHECK(out.empty()); // same item: no second hover
    f.HoveredId = 4; f.MouseDown = true; step(f); CHECK(!has(Event::Hover)); // never while a button is down
    f.MouseDown = false; f.MouseReleased = true; f.PopupDepth = 0; step(f); CHECK(has(Event::Click) && !has(Event::MenuClose));
    f.MouseReleased = false; f.HoverInMenu = f.HoverInPickMenu = false; f.HoveredId = 0;
    f.PopupDepth = 1; step(f);
    f.PopupDepth = 0; step(f); CHECK(has(Event::MenuClose));
    f.HoveredId = 11; f.MouseDx = 3.0f; step(f); CHECK(!has(Event::Hover)); // a panel row: no hover sound

    // Drag & drop (P8): it sounds on the release frame. Observed after ImGui::EndFrame, a delivered drop already reads let go
    // there; one let go elsewhere still reads held until the next frame, which stays quiet.
    f.DragActive = true; step(f); CHECK(has(Event::DragStart));
    f.DropTargetHovered = true; step(f); CHECK(out.empty());
    f.MouseReleased = f.DragReleased = true; f.DragActive = false; f.DropTargetHovered = false; step(f);
    CHECK(has(Event::DropAccept) && out.size() == 1);
    f.MouseReleased = f.DragReleased = false; step(f); CHECK(out.empty());
    f.DragActive = true; step(f); CHECK(has(Event::DragStart));
    f.MouseReleased = f.DragReleased = true; step(f); CHECK(has(Event::DropReject) && out.size() == 1); // still held
    f.MouseReleased = f.DragReleased = false; step(f); CHECK(out.empty());
    f.DragActive = false; step(f); CHECK(out.empty()); // ImGui lets it go a frame later: said already
    // Observed before EndFrame (Delivery still set) the delivered drop also reads held on its release frame.
    f.DragActive = true; step(f);
    f.DropTargetHovered = true; step(f);
    f.MouseReleased = f.DragReleased = f.DropAccepted = true; step(f); CHECK(has(Event::DropAccept) && out.size() == 1);
    f.MouseReleased = f.DragReleased = f.DropAccepted = false; f.DragActive = f.DropTargetHovered = false; step(f); CHECK(out.empty());
    // A drag that ends without a release seen (the payload elapsed) still sounds, on the edge.
    f.DragActive = true; step(f);
    f.DragActive = false; step(f); CHECK(has(Event::DropReject));
    f.MouseDx = 0.0f; f.HoveredId = 0;

    // Text fields (P7): an edit confirmed with Enter commits, abandoned with Escape cancels; clicking away, Tab, and leaving
    // without an edit stay silent. ImGui drops the active id on the Enter / Escape frame while the key still reads pressed.
    auto typeInto = [&](unsigned id) {
        f.ActiveId = id; f.TextActive = true; f.Edited = false; step(f); CHECK(out.empty());
        f.Edited = true; step(f); CHECK(out.empty()); // typing: quiet (no mouse down, so not a slide either)
        f.Edited = false; step(f); CHECK(out.empty());
    };
    auto leave = [&](bool enter, bool esc) {
        f.ActiveId = 0; f.TextActive = false; f.EnterPressed = enter; f.EscPressed = esc; step(f);
        f.EnterPressed = f.EscPressed = false;
    };
    typeInto(21); leave(true, false); CHECK(has(Event::FieldCommit) && out.size() == 1);
    typeInto(22); leave(false, true); CHECK(has(Event::FieldCancel) && out.size() == 1);
    typeInto(23); leave(false, false); CHECK(out.empty()); // clicked away / Tab
    f.ActiveId = 24; f.TextActive = true; step(f); leave(true, false); CHECK(out.empty()); // Enter without an edit
    // Tab straight into the next field: the first one ends silently and the second commits on its own Enter.
    typeInto(25); f.ActiveId = 26; f.Edited = false; step(f); CHECK(out.empty());
    f.Edited = true; step(f); f.Edited = false; leave(true, false); CHECK(has(Event::FieldCommit));
    // A rename field in a popup: the Enter that commits it also closes the popup, which doesn't click on top.
    f.PopupDepth = 1; step(f); CHECK(has(Event::MenuOpen));
    f.HoverInPickMenu = true; f.HoveredId = 5;
    typeInto(27); f.PopupDepth = 0; leave(true, false); CHECK(has(Event::FieldCommit) && !has(Event::Click) && !has(Event::MenuClose));
}

// P8: the primitives sound on the press; a press dragged off and let go elsewhere takes it back with a soft, lower ClickCancel.
void TestEditorSoundPressCancel() {
    using EditorSound::UiFrame;
    using EditorSound::UiObserver;
    using EditorSound::UiSound;
    UiObserver o;
    std::vector<UiSound> out;
    auto step = [&](const UiFrame& f) { out.clear(); o.Step(f, out); };
    UiFrame f;
    step(f); // baseline
    auto press = [&](unsigned id, bool noted) {
        f.ActiveId = id; f.MouseDown = true; f.PressId = noted ? id : 0; f.HoveredId = id; step(f); CHECK(out.empty());
        f.PressId = 0; step(f); CHECK(out.empty()); // held
    };
    auto release = [&](bool overIt) {
        f.ActiveId = 0; f.MouseDown = false; f.MouseReleased = true; f.ReleasedOnItem = overIt; f.HoveredId = overIt ? 7 : 0; step(f);
        f.MouseReleased = f.ReleasedOnItem = false;
    };
    // Dragged off: one quiet, lower ClickCancel.
    press(7, true); f.HoveredId = 0; step(f); release(false);
    CHECK(out.size() == 1 && out[0].E == Event::ClickCancel);
    CHECK(out.size() == 1 && std::fabs(out[0].Pitch - UiObserver::kCancelPitch) < 1e-6f && out[0].GainDb < -3.0f);
    step(f); CHECK(out.empty()); // once
    // Let go on it: it clicked, nothing to take back.
    press(7, true); release(true); CHECK(out.empty());
    // A press that didn't sound (a ScopedClickSilence, or not a primitive) has nothing to take back.
    press(7, false); release(false); CHECK(out.empty());
    // A keyboard activation: noted, let go without a mouse release.
    f.ActiveId = 7; f.PressId = 7; step(f); f.PressId = 0;
    f.ActiveId = 0; step(f); CHECK(out.empty());
    // A press that became a drag & drop: its drop speaks, not a cancel.
    press(7, true); f.DragActive = true; step(f); CHECK(std::none_of(out.begin(), out.end(), [](const UiSound& u) { return u.E == Event::ClickCancel; }));
    f.ActiveId = 0; f.MouseDown = false; f.MouseReleased = f.DragReleased = true; step(f);
    CHECK(std::none_of(out.begin(), out.end(), [](const UiSound& u) { return u.E == Event::ClickCancel; }));
}

// P8: a primitive's press in a menu already clicked: the pick that closes the menu doesn't click again (another item does).
void TestEditorSoundPressInMenu() {
    using EditorSound::UiFrame;
    using EditorSound::UiObserver;
    using EditorSound::UiSound;
    UiObserver o;
    std::vector<UiSound> out;
    auto step = [&](const UiFrame& f) { out.clear(); o.Step(f, out); };
    auto has = [&](Event e) { return std::any_of(out.begin(), out.end(), [e](const UiSound& u) { return u.E == e; }); };
    UiFrame f;
    step(f);
    f.PopupDepth = 1; step(f); CHECK(has(Event::MenuOpen));
    f.HoverInMenu = f.HoverInPickMenu = true; f.HoveredId = 3;
    f.ActiveId = 3; f.MouseDown = true; f.PressId = 3; step(f); f.PressId = 0;
    f.ActiveId = 0; f.MouseDown = false; f.MouseReleased = true; f.ReleasedOnItem = true; f.PopupDepth = 0; step(f);
    CHECK(!has(Event::Click) && !has(Event::MenuClose) && !has(Event::ClickCancel));
    f.MouseReleased = f.ReleasedOnItem = false;
    f.PopupDepth = 1; step(f);
    f.MouseReleased = true; f.PopupDepth = 0; step(f); CHECK(has(Event::Click)); // a plain menu item: the observer clicks
}

// P8: the gain table is the dB trims, exactly; the service is Active only when there is something to hear.
void TestEditorSoundGainTableAndActive() {
    for (int i = 0; i < EditorSoundEvents::kEventCount; ++i)
        for (int r = 0; r < 8; ++r) {
            const float db = EditorSound::Info((Event)i).GainDb + std::max(Scheduler::kRepeatStepDb * (float)r, Scheduler::kRepeatFloorDb);
            const float want = std::pow(10.0f, db / 20.0f);
            CHECK(std::fabs(EditorSound::EventGain((Event)i, r) - want) <= want * 1e-6f);
        }
    // Decide's gains come off it: a spaced-out Hover is its trim, the floor is the trim plus kRepeatFloorDb.
    Scheduler sch(3u);
    Voice v;
    Settings st;
    sch.SetVariantCount(Event::Hover, 1);
    CHECK(sch.Decide(Event::Hover, 0.0, 0.5f, true, st, v) && std::fabs(v.Gain - EditorSound::EventGain(Event::Hover)) < 1e-7f);

    // No files needed: Init with no folders still makes the service ready.
    EditorSound::Shutdown();
    CHECK(!EditorSound::Active());
    Settings on;
    EditorSound::Apply(on);
    CHECK(!EditorSound::Active()); // not initialised
    EditorSound::Init("", "");
    CHECK(EditorSound::Active());
    Settings off = on;
    off.Enabled = false;
    EditorSound::Apply(off);
    CHECK(!EditorSound::Active());
    off = on;
    off.Master = 0.0f;
    EditorSound::Apply(off);
    CHECK(!EditorSound::Active());
    EditorSound::Apply(on);
    CHECK(EditorSound::Active());
    const int played = EditorSound::PlayedCount();
    EditorSound::Play(Event::Click); // no files: nothing plays, nothing breaks
    CHECK(EditorSound::PlayedCount() == played);
    EditorSound::Shutdown();
    CHECK(!EditorSound::Active());
    EditorSound::Apply(EditorSound::Settings{});
}

// The transform gizmo: grab, a tick per snap step landed (or per stretch of free travel), release.
void TestEditorSoundGizmo() {
    using EditorSound::GizmoTracker;
    using EditorSound::UiSound;
    GizmoTracker g;
    std::vector<UiSound> out;
    auto count = [&](Event e) { return (int)std::count_if(out.begin(), out.end(), [e](const UiSound& u) { return u.E == e; }); };
    g.Step(false, false, true, 0, 0, 0.5f, out);
    CHECK(out.empty());
    g.Step(true, false, true, 0, 0, 0.5f, out);
    CHECK(count(Event::SliderGrab) == 1);
    out.clear();
    // Snapping: only the frames that land on a new step tick.
    for (int i = 0; i < 10; ++i) g.Step(true, i % 3 == 0, true, 3.0f, 0, 0.5f, out);
    CHECK(count(Event::SliderTick) == 4);
    out.clear();
    // Free: one tick per kFreeTickPx of travel while it moves.
    for (int i = 0; i < 20; ++i) g.Step(true, true, false, 2.0f, 0, 0.5f, out);
    CHECK(count(Event::SliderTick) == (int)(40.0f / GizmoTracker::kFreeTickPx));
    out.clear();
    g.Step(false, false, false, 0, 0, 0.5f, out);
    CHECK(count(Event::SliderRelease) == 1 && out.size() == 1);
}

// Both sets load; the premium one plays only where the user opted in and it has the event.
void TestEditorSoundPremiumSet() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "tartarus_editor_sets_ut";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "builtin", ec);
    fs::create_directories(base / "premium", ec);
    WriteToneWav(base / "builtin" / "Click_1.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(base / "builtin" / "Click_2.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(base / "builtin" / "Save.wav", 48000, 0.05f, 0.1f);
    WriteToneWav(base / "premium" / "Click.wav", 48000, 0.05f, 0.1f);
    EditorSound::Settings st;
    EditorSound::Apply(st); // UsePremium off (the default)
    EditorSound::Init((base / "builtin").string(), (base / "premium").string());
    CHECK(EditorSound::PremiumEventCount() == 1); // found, though not decoded while the set is off
    CHECK(EditorSound::BuiltInFiles(Event::Click).size() == 2 && EditorSound::PremiumFiles(Event::Click).size() == 1);
    CHECK(!EditorSound::IsOverride(Event::Click) && EditorSound::VariantCount(Event::Click) == 2);
    st.UsePremium = true;
    EditorSound::Apply(st);
    CHECK(EditorSound::IsOverride(Event::Click) && EditorSound::VariantCount(Event::Click) == 1);
    CHECK(!EditorSound::IsOverride(Event::Save) && EditorSound::VariantCount(Event::Save) == 1); // no premium Save: built-in
    st.UsePremium = false;
    EditorSound::Apply(st);
    CHECK(!EditorSound::IsOverride(Event::Click) && EditorSound::VariantCount(Event::Click) == 2);
    EditorSound::Shutdown();
    EditorSound::Apply(EditorSound::Settings{});
    fs::remove_all(base, ec);
}

// Every event has a built-in file in git, and none can clip the editor's output at the default level (no limiter on it).
void TestEditorSoundBuiltInSetComplete() {
    namespace fs = std::filesystem;
    const std::string dir = (fs::u8path(TARTARUS_SOURCE_DIR) / "resources" / "EditorSounds").u8string();
    const float master = EditorSound::Settings{}.Master;
    CHECK(std::fabs(master - 0.70f) < 1e-6f); // the P6 remaster's default
    float worstDb = -1000.0f;
    for (int i = 0; i < EditorSoundEvents::kEventCount; ++i) {
        const auto files = EditorSound::FindVariants(dir, (Event)i);
        CHECK(!files.empty());
        for (const std::string& f : files) {
            std::ifstream in(fs::u8path(f), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            int peak = 0;
            for (size_t p = 12; p + 8 <= bytes.size();) { // walk the RIFF chunks to "data"
                std::uint32_t len = 0;
                std::memcpy(&len, &bytes[p + 4], 4);
                if (std::memcmp(&bytes[p], "data", 4) == 0) {
                    for (size_t s = p + 8; s + 1 < std::min(bytes.size(), p + 8 + len); s += 2) {
                        std::int16_t v;
                        std::memcpy(&v, &bytes[s], 2);
                        peak = std::max(peak, std::abs((int)v));
                    }
                    break;
                }
                p += 8 + len + (len & 1);
            }
            CHECK(peak > 0);
            const float db = 20.0f * std::log10(std::max(peak, 1) / 32768.0f) + EditorSound::Info((Event)i).GainDb;
            worstDb = std::max(worstDb, db);
        }
    }
    CHECK(worstDb + 20.0f * std::log10(master) < -3.0f); // headroom even at full category volume
}

// The P6 mastering rules, on the committed files: every one starts and ends at silence (a sound that starts or stops
// mid-waveform clicks), and none is buried - each is within reach of the loudest of its kind (the first set had files 8 dB
// under their neighbours).
void TestEditorSoundBuiltInSetMastered() {
    namespace fs = std::filesystem;
    const std::string dir = (fs::u8path(TARTARUS_SOURCE_DIR) / "resources" / "EditorSounds").u8string();
    float quietestRms = 0.0f, loudestRms = -1000.0f;
    for (int i = 0; i < EditorSoundEvents::kEventCount; ++i) {
        for (const std::string& f : EditorSound::FindVariants(dir, (Event)i)) {
            std::ifstream in(fs::u8path(f), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::vector<std::int16_t> pcm;
            for (size_t p = 12; p + 8 <= bytes.size();) {
                std::uint32_t len = 0;
                std::memcpy(&len, &bytes[p + 4], 4);
                if (std::memcmp(&bytes[p], "data", 4) == 0) {
                    pcm.resize(std::min<size_t>(len, bytes.size() - p - 8) / 2);
                    std::memcpy(pcm.data(), &bytes[p + 8], pcm.size() * 2);
                    break;
                }
                p += 8 + len + (len & 1);
            }
            CHECK(pcm.size() > 100);
            if (pcm.size() <= 100) continue;
            CHECK(std::abs((int)pcm.front()) < 66); // 0.002 of full scale
            CHECK(std::abs((int)pcm.back()) < 33);  // 0.001
            double sum = 0.0;
            for (std::int16_t s : pcm) sum += (double)s * s;
            const float rmsDb = 10.0f * std::log10((float)(sum / (double)pcm.size()) / (32768.0f * 32768.0f));
            quietestRms = std::min(quietestRms, rmsDb);
            loudestRms = std::max(loudestRms, rmsDb);
        }
    }
    CHECK(quietestRms > -30.0f);              // nothing buried (the first set went down to -37 dBFS RMS)
    CHECK(loudestRms - quietestRms < 12.0f);  // ticks sit under notifications, but within reach
}

// The action sounds that ride on an undo step's label.
void TestEditorSoundUndoLabels() {
    CHECK(EditorSound::UndoLabelEvent("Create Cube") == Event::Create);
    CHECK(EditorSound::UndoLabelEvent("Place Prefab Instance") == Event::Create);
    CHECK(EditorSound::UndoLabelEvent("Add Rigidbody") == Event::ComponentAdd);
    CHECK(EditorSound::UndoLabelEvent("Remove Collider") == Event::ComponentRemove);
    CHECK(EditorSound::UndoLabelEvent("Paste Component Values") == Event::Paste);
    CHECK(EditorSound::UndoLabelEvent("Apply to Prefab") == Event::PrefabApply);
    CHECK(EditorSound::UndoLabelEvent("Revert to Prefab") == Event::PrefabRevert);
    CHECK(EditorSound::UndoLabelEvent("Paste") == Event::Count);  // the entity Paste plays its own, once it worked
    CHECK(EditorSound::UndoLabelEvent("Rename") == Event::Count); // the Inspector pushes it when the field is clicked into
    CHECK(EditorSound::UndoLabelEvent("Move") == Event::Count);
    CHECK(EditorSound::UndoLabelEvent("Delete") == Event::Count);
    CHECK(EditorSound::UndoLabelEvent("Delete Assets") == Event::Delete);
    CHECK(EditorSound::UndoLabelEvent("Duplicate Asset") == Event::Duplicate);
    CHECK(EditorSound::UndoLabelEvent("Rename Folder") == Event::Rename);
    CHECK(EditorSound::UndoLabelEvent("Move Asset to Folder") == Event::AssetMove);
    CHECK(EditorSound::UndoLabelEvent("Move Folder") == Event::AssetMove);
    CHECK(EditorSound::UndoLabelEvent("Move Vertex") == Event::Count); // a mesh edit, not an asset move
    CHECK(EditorSound::UndoLabelEvent("Reset Transform") == Event::ResetDefault);
    CHECK(EditorSound::UndoLabelEvent("Reimport Texture") == Event::AssetReimport);
    CHECK(EditorSound::UndoLabelEvent("Align Camera to View") == Event::ViewPreset);
}

// Which apps the muter matches, and the Preferences field's list.
void TestAppAudioMuterNames() {
    const std::vector<std::string> names{"Mechvibes.exe"};
    CHECK(AppAudioMuter::NameMatches("C:\\Users\\x\\AppData\\Local\\Programs\\Mechvibes\\Mechvibes.exe", names));
    CHECK(AppAudioMuter::NameMatches("D:/apps/MECHVIBES.EXE", names));
    CHECK(!AppAudioMuter::NameMatches("C:\\Windows\\explorer.exe", names));
    CHECK(!AppAudioMuter::NameMatches("C:\\x\\NotMechvibes.exe", names));
    CHECK(!AppAudioMuter::NameMatches("", names));
    CHECK(AppAudioMuter::NameMatches("C:\\x\\mechvibes.exe", {"Mechvibes"})); // .exe optional
    const auto parsed = AppAudioMuter::ParseNameList(" Mechvibes.exe, klack.exe;;Mechvibes.exe , ");
    CHECK(parsed.size() == 2 && parsed[0] == "Mechvibes.exe" && parsed[1] == "klack.exe");
    CHECK(AppAudioMuter::JoinNameList(parsed) == "Mechvibes.exe, klack.exe");
    CHECK(AppAudioMuter::ParseNameList("").empty());
}

} // namespace

void RegisterEditorSoundTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Editor sounds: every event named, notifications and Play mode centred", TestEditorSoundEventTable);
    tests.emplace_back("Editor sounds: variants, cooldown, rapid repeats, pan, focus and switches", TestEditorSoundSchedulerRules);
    tests.emplace_back("Editor sounds: a single file or numbered variants", TestEditorSoundFindVariants);
    tests.emplace_back("Editor sounds: sliders, menus, hover and drag & drop read off ImGui's state", TestEditorSoundUiObserver);
    tests.emplace_back("Editor sounds: a press dragged off a button is taken back softly", TestEditorSoundPressCancel);
    tests.emplace_back("Editor sounds: a primitive's press in a menu doesn't click again on the pick", TestEditorSoundPressInMenu);
    tests.emplace_back("Editor sounds: the gain table matches the dB trims; Active() gating", TestEditorSoundGainTableAndActive);
    tests.emplace_back("Editor sounds: the transform gizmo's grab, snap ticks and release", TestEditorSoundGizmo);
    tests.emplace_back("Editor sounds: the premium Drive set is opt-in, event by event", TestEditorSoundPremiumSet);
    tests.emplace_back("Editor sounds: every event has a built-in file, with headroom at the default level",
                       TestEditorSoundBuiltInSetComplete);
    tests.emplace_back("Editor sounds: every built-in file starts and ends at silence, none buried",
                       TestEditorSoundBuiltInSetMastered);
    tests.emplace_back("Editor sounds: Create, Add / Remove Component, Paste and prefab actions off undo labels",
                       TestEditorSoundUndoLabels);
    tests.emplace_back("App audio muter: exe-name matching and the name list", TestAppAudioMuterNames);
    tests.emplace_back("Editor sounds: their own output, outside the game's mix, Mute and pause", TestEditorSoundSeparateOutput);
}
