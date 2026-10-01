#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The squad's radio: Combine-style coded barks (assets/Audio/Voice/combine/, made by
// tools/gen_combine_voice.py). One channel per squad - a single speaker at a time - with priorities
// (a higher one cuts a lower one off mid-line), a per-event cooldown, and an optional "copy" from a
// squadmate after the line ends. Lines play positional on the Voice bus from the speaker.
//
// The scheduler is pure (time comes in as `now`), so it runs in the unit tests without an audio
// device; with the audio engine up it also starts and stops the clips. Every line it starts is kept in
// History() (with the time it was cut, if it was), which tests check for overlaps.

enum class Bark : std::uint8_t {
    Contact,       // first sight of the player
    ContactRelay,  // told by a squadmate
    Gunfire,       // heard shots
    FlankLeft,
    FlankRight,
    MovingUp,
    FallingBack,
    LostTarget,
    ManDown,
    Wounded,
    Reloading,
    Covering,      // suppressing
    TargetDown,    // the player is dead
    PlayerHurt,    // a round of ours landed
    Suspicious,    // stealth: something is off
    Investigating, // stealth: going to look
    AllClear,      // stealth: nothing found
    Idle,          // chatter
    Melee,
    Copy,          // acknowledgement
    Count
};
const char* BarkKey(Bark b); // manifest key, e.g. "man_down"

// Built-in numbers (priority, cooldown, a text) used when the manifest has none for an event.
struct BarkDefault { int Priority; float Cooldown; const char* Text; bool Responds; };
const BarkDefault& BarkDefaults(Bark b);

struct BarkPlayed {
    int Squad = 0, Speaker = -1, Unit = 0, Voice = 0;
    Bark Event = Bark::Contact;
    bool Responder = false;
    std::string Text, File;
    glm::vec3 Pos{0.0f};
    float Start = 0.0f, End = 0.0f; // End = Start + the clip's length
    float Cut = -1.0f;              // when a higher-priority line preempted it (< 0: it finished)
    int Priority = 0;
    float EffectiveEnd() const { return Cut >= 0.0f ? Cut : End; }
};

class SquadVoice {
public:
    SquadVoice();
    // Reads <dir>/manifest.json (and preloads the clips when the audio engine is up). False if missing:
    // the built-in texts still drive subtitles and the log, just silently.
    bool LoadManifest(const std::string& dir);
    bool Loaded() const { return m_Loaded; }
    // Forgets the channels' state and the history (a new Play).
    void Reset();
    // Audio off (tests) or on; on by default, though nothing plays unless the audio engine is up.
    void SetAudio(bool on) { m_Audio = on; }
    // Where the audio is (rolloff metres).
    void SetRange(float nearM, float farM) { m_Near = nearM; m_Far = farM; }

    // `speaker` (an Npc index) asks to say `ev` at `now`. Returns true when the line started: false when
    // the event is cooling down, or the channel is busy with an equal or higher priority.
    // `responder` (-1: none) is a squadmate who may answer "copy" when this line ends.
    bool Say(float now, int squad, int speaker, int unit, Bark ev, const glm::vec3& pos, int responder = -1,
             int responderUnit = 0, const glm::vec3& responderPos = glm::vec3(0.0f));
    // Fires due responders and tidies up. Call every frame.
    void Update(float now);

    bool Busy(int squad, float now) const;
    // Lines started since the last call (for subtitles).
    std::vector<BarkPlayed> TakeSubtitles();
    const std::vector<BarkPlayed>& History() const { return m_History; }
    int Spoken() const { return m_Spoken; }
    int Cuts() const { return m_Cuts; }
    // Tests: the first two lines of one squad that overlap in time (not counting a cut line up to where
    // it was cut), or -1.
    int FirstOverlap(int squad) const;
    int Priority(Bark ev) const;
    float Cooldown(Bark ev) const;

private:
    struct Line { std::string Text; std::string File[2]; float Duration[2] = {0.0f, 0.0f}; };
    struct EventDef { int Priority = 0; float Cooldown = 0.0f; std::vector<Line> Lines; bool FromManifest = false; };
    struct Channel {
        float BusyUntil = -1e9f;
        int Index = -1;               // history index of the line on air
        std::uint32_t Handle = 0;     // AudioEngine voice
        bool RespPending = false;
        float RespAt = 0.0f;
        int RespSpeaker = -1, RespUnit = 0;
        glm::vec3 RespPos{0.0f};
        int LastLine[(int)Bark::Count] = {};
        float LastEvent[(int)Bark::Count];
    };
    Channel& Chan(int squad);
    bool Start(float now, int squad, int speaker, int unit, Bark ev, const glm::vec3& pos, bool responder);
    float Rand01();

    std::vector<EventDef> m_Events;
    std::vector<Channel> m_Channels;
    std::vector<BarkPlayed> m_History;
    std::vector<BarkPlayed> m_Pending; // not yet taken as subtitles
    std::string m_Dir;
    bool m_Loaded = false, m_Audio = true;
    float m_Near = 7.0f, m_Far = 70.0f;
    std::uint32_t m_Rng = 0xBA4Cu;
    int m_Spoken = 0, m_Cuts = 0;
    static constexpr float kGap = 0.18f;      // silence on the channel after a line
    static constexpr float kCopyDelay = 0.3f;
};
