#pragma once

#include <string>

// Every environment variable the engine reads, in one table: its name, what kind of value it takes and what it does.
// `--help` prints the table. Read them through Env:: (never getenv/_dupenv_s directly) so a new switch shows up here.
//
// Reads are live (the environment can change under a unit test that sets TARTARUS_MUTE), so a hot path keeps its own
// `static const` copy, as it always has.
//
// Flags are on when the variable is set to anything but "" or "0".

// X(Id, "NAME", Kind, "help")   Kind: Flag | Int | Float | Str | List (';'-separated)
#define TARTARUS_ENV_VARS(X)                                                                                           \
    /* Harness runs */                                                                                                  \
    X(Hidden, "TARTARUS_HIDDEN", Flag, "keep the window hidden during harness runs")                                  \
    X(Mute, "TARTARUS_MUTE", Flag, "start with the master bus muted")                                                 \
    X(WeaponLab, "TARTARUS_WEAPON_LAB", Flag, "same as --weapon-lab (run-weapon-lab.cmd)")                            \
    X(SeeThrough, "TARTARUS_SEE_THROUGH", Flag, "force the see-through (transparent) editor window")                  \
    X(PerfWarmup, "TARTARUS_PERF_WARMUP", Int, "--perf-bench warm-up frames before each phase")                       \
    X(MemReport, "TARTARUS_MEM_REPORT", Str, "smoke harness: write per-scene model memory snapshots to this folder")  \
    X(OpenAll, "TARTARUS_OPEN_ALL", Flag, "smoke shots: open every door/drawer on the first frame")                   \
    X(ShotCameras, "TARTARUS_SHOT_CAMERAS", List, "--smoke-shots: 'x y z yaw pitch;...' camera poses to capture")      \
    X(ShotStart, "TARTARUS_SHOT_START", Int, "--smoke-shots: frame of the first shot-camera capture (default 40)")    \
    X(ShotFov, "TARTARUS_SHOT_FOV", Float, "--smoke-shots: Scene-view FOV for shot cameras")                          \
    X(EditorTestInstance, "TARTARUS_EDITOR_TEST_INSTANCE", Str, "--editor-tests: scratch folder suffix")              \
    X(EditorTestScale, "TARTARUS_EDITOR_TEST_SCALE", Float, "--editor-tests: UI scale override")                      \
    X(EditorTestSize, "TARTARUS_EDITOR_TEST_SIZE", Str, "--editor-tests: window size WxH")                            \
    X(EditorTestShow, "TARTARUS_EDITOR_TEST_SHOW", Flag, "--editor-tests: keep the window shown")                     \
    X(EditorTestDeadline, "TARTARUS_EDITOR_TEST_DEADLINE", Float, "--editor-tests: watchdog deadline in minutes")     \
    X(EditorTestShots, "TARTARUS_EDITOR_TEST_SHOTS", Str, "--editor-tests: folder for failure screenshots")           \
    X(EditorTestExclude, "TARTARUS_EDITOR_TEST_EXCLUDE", List, "--editor-tests: test names to skip")                  \
    X(EditorTestShard, "TARTARUS_EDITOR_TEST_SHARD", Str, "--editor-tests: run shard i/n")                            \
    X(EditorTestDump, "TARTARUS_EDITOR_TEST_DUMP", Str, "--editor-tests: on failure, dump this window's widgets")     \
    X(MotionDemo, "TARTARUS_MOTION_DEMO", Str, "MotionDemo: output folder")                                           \
    X(Ffmpeg, "TARTARUS_FFMPEG", Str, "MotionDemo: ffmpeg.exe path")                                                  \
    /* NPC tests */                                                                                                     \
    X(NpcTestScene, "NPC_TEST_SCENE", Str, "--npc-test: scene name under scenes/ instead of the default")             \
    X(NpcTestRoom, "NPC_TEST_ROOM", Str, "--npc-test: Apartment room (302bed|303living|303bed2)")                     \
    X(NpcTestSeconds, "NPC_TEST_SECONDS", Float, "--npc-test: run length override")                                   \
    X(NpcTestRecord, "NPC_TEST_RECORD", Str, "--npc-test: record frames to this folder")                              \
    X(NpcTestBare, "NPC_TEST_BARE", Flag, "--npc-test: spawn NPCs without outfits")                                   \
    X(NpcTestDoorsOnly, "NPC_TEST_DOORS_ONLY", Str, "--npc-test doors: only doors whose name contains this")          \
    X(NpcTestDoorsTrace, "NPC_TEST_DOORS_TRACE", Flag, "--npc-test doors: trace")                                     \
    X(NpcTestRoamPoints, "NPC_TEST_ROAM_POINTS", List, "--npc-test roam: 'x y z;...' points to walk")                 \
    X(NpcTestRoamStraight, "NPC_TEST_ROAM_STRAIGHT", Flag, "--npc-test roam: walk straight, no route")                \
    X(NpcTestRoamTrace, "NPC_TEST_ROAM_TRACE", Flag, "--npc-test roam: trace feet")                                   \
    X(DecalStress, "TARTARUS_DECAL_STRESS", Flag, "--npc-test: decal stress mode")                                    \
    /* Traces and probes */                                                                                             \
    X(SpiderTrace, "TARTARUS_SPIDER_TRACE", Flag, "trace spider AI, movement and ragdolls")                           \
    X(FoleyFeetLog, "FOLEY_FEET_LOG", Flag, "trace footstep foley")                                                    \
    X(MoverTrace, "MOVER_TRACE", Flag, "unit tests: trace the character mover")                                       \
    X(FootSlideProbe, "TARTARUS_FOOT_SLIDE_PROBE", Int, "IK foot-slide probe: 1 on, 0 off, unset = default")          \
    X(PumpTrace, "TARTARUS_PUMP_TRACE", Flag, "trace the asset library pump")                                         \
    X(ProbeLog, "TARTARUS_PROBE_LOG", Flag, "log reflection probe captures")                                          \
    X(ProbeDump, "TARTARUS_PROBE_DUMP", Str, "dump reflection probe faces to this folder")                            \
    X(SkinAudit, "TARTARUS_SKIN_AUDIT", Flag, "log skinning audit")                                                   \
    X(TrisReport, "TARTARUS_TRIS_REPORT", Int, "print a triangle report on this frame")                               \
    X(HizStats, "TARTARUS_HIZ_STATS", Flag, "print Hi-Z culling stats")                                               \
    X(SampleHitchMs, "TARTARUS_SAMPLE_HITCH_MS", Float, "--perf-sample: keep only frames over this many ms")          \
    X(SampleFocus, "TARTARUS_SAMPLE_FOCUS", Str, "--perf-sample: report call chains through this function")           \
    X(SampleUnder, "TARTARUS_SAMPLE_UNDER", List, "--perf-sample: report time under these functions")                 \
    X(NodesPivots, "TARTARUS_NODES_PIVOTS", Flag, "--model-nodes: keep FBX pivot nodes")                              \
    X(GlDebug, "TARTARUS_GL_DEBUG", Flag, "GL debug output in Release builds")                                                     \
    X(ForceFboIncomplete, "TARTARUS_FORCE_FBO_INCOMPLETE", Str, "test hook: fail FBOs whose owner contains this (set but empty: all)")         \
    X(GlassFullCapture, "TARTARUS_GLASS_FULL_CAPTURE", Flag, "editor glass: capture the full window")                 \
    /* Kill switches */                                                                                                 \
    X(NoHiz, "TARTARUS_NO_HIZ", Flag, "disable Hi-Z occlusion culling")                                               \
    X(NoMdi, "TARTARUS_NO_MDI", Flag, "disable multi-draw indirect")                                                  \
    X(NoMeshArena, "TARTARUS_NO_MESH_ARENA", Flag, "disable the static mesh arena")                                   \
    X(NoSkinArena, "TARTARUS_NO_SKIN_ARENA", Flag, "disable the skinned mesh arena")                                  \
    X(NoPreskin, "TARTARUS_NO_PRESKIN", Flag, "disable compute pre-skinning")                                         \
    X(NoAsyncUpload, "TARTARUS_NO_ASYNC_UPLOAD", Flag, "disable async texture/mesh uploads")                          \
    X(NoEngineVariants, "TARTARUS_NO_ENGINE_VARIANTS", Flag, "disable engine shader variants")                        \
    X(NoFur, "TARTARUS_NO_FUR", Flag, "disable fur rendering")                                                        \
    X(NoSubstrate, "TARTARUS_NO_SUBSTRATE", Flag, "disable decal substrate")                                          \
    X(NoAnimPrefetch, "TARTARUS_NO_ANIM_PREFETCH", Flag, "disable animation clip prefetch")                           \
    X(NoUiThrottle, "TARTARUS_NO_UI_THROTTLE", Flag, "disable the asset browser UI throttle")                         \
    /* Paths and audio */                                                                                               \
    X(AssetRoot, "TARTARUS_ASSET_ROOT", Str, "engine asset folder override")                                          \
    X(AudioCapture, "AUDIO_CAPTURE", Str, "write the audio output to this WAV file")                                  \
    X(LocalAppData, "LOCALAPPDATA", Str, "(Windows) user data root")

namespace Env {

enum class Kind { Flag, Int, Float, Str, List };

enum class Var {
#define TARTARUS_ENV_ENUM(id, name, kind, help) id,
    TARTARUS_ENV_VARS(TARTARUS_ENV_ENUM)
#undef TARTARUS_ENV_ENUM
    Count
};

struct Info {
    const char* Name;
    Kind Type;
    const char* Help;
};
const Info& Describe(Var v);

// Whether the variable is set at all (even to "0").
bool IsSet(Var v);
// The raw value, or `fallback` when unset.
std::string Str(Var v, const std::string& fallback = {});
// Set to anything but "" or "0".
bool Flag(Var v);
// The value as a number, or `fallback` when unset or not a number.
int Int(Var v, int fallback);
float Float(Var v, float fallback);

// Tests only: sets (or, with nullptr, clears) the variable for this process.
void Set(Var v, const char* value);

// The table, for --help.
std::string HelpText();

} // namespace Env
