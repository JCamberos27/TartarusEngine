#include "App/LaunchOptions.h"

#include "Core/Env.h"
#include "ProjectPaths.h"

#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>

namespace {

enum class Arity {
    None,     // --flag
    Optional, // --flag [value]: the next argument, unless it starts with '-'
    Required, // --flag <value>
    Many,     // --flag <value>...: every following argument that doesn't start with '-'
};

constexpr unsigned Bit(LaunchMode m) { return 1u << (unsigned)m; }
constexpr unsigned kGuiModes = Bit(LaunchMode::Editor) | Bit(LaunchMode::SmokeTest) | Bit(LaunchMode::PerfBench) |
                               Bit(LaunchMode::NpcTest) | Bit(LaunchMode::EditorTests) | Bit(LaunchMode::EditorShot);
constexpr unsigned kAnyMode = ~0u;

using Apply = void (*)(LaunchOptions&, const std::string&);

struct FlagDef {
    const char* Name;
    Arity Takes;
    const char* Value; // shown in --help
    bool IsMode;       // true: `Mode` is the run's mode; false: a modifier allowed in `Modes`
    LaunchMode Mode;
    unsigned Modes;
    Apply Set;
    const char* Help;
};

constexpr LaunchMode kNoMode = LaunchMode::Editor;

// clang-format off
const FlagDef kFlags[] = {
    // Modes: at most one per run.
    {"--smoke-test", Arity::Optional, "dir", true, LaunchMode::SmokeTest, 0,
     [](LaunchOptions& o, const std::string& v) { o.smokeScenesDirArg = v; },
     "render every scene in dir (default: the engine's test scenes) and fail on GL errors"},
    {"--perf-bench", Arity::Optional, "dir|scene.json", true, LaunchMode::PerfBench, 0,
     [](LaunchOptions& o, const std::string& v) { o.smokeScenesDirArg = v; },
     "time each scene: edit mode, docked Play, maximized Play; CPU/GPU breakdown"},
    {"--npc-test", Arity::Optional, "scenario", true, LaunchMode::NpcTest, 0,
     [](LaunchOptions& o, const std::string& v) { o.npcTestScenario = v; },
     "scripted NPC scenario (watch, fight, die, ...); exit 0 when every check passed"},
    {"--editor-tests", Arity::Optional, "filter", true, LaunchMode::EditorTests, 0,
     [](LaunchOptions& o, const std::string& v) { o.editorTestsFilter = v; },
     "end-to-end editor UI tests in a hidden window and a scratch user folder"},
    {"--editor-shot", Arity::Required, "dir", true, LaunchMode::EditorShot, 0,
     [](LaunchOptions& o, const std::string& v) { o.editorShotDir = v; },
     "save PNGs of fixed editor views (on a copy of your prefs), then exit"},
    {"--build", Arity::Optional, "outDir", true, LaunchMode::Build, 0,
     [](LaunchOptions& o, const std::string& v) { o.buildOutArg = v; },
     "File > Build without the UI; outDir overrides the Output Folder"},
    {"--unit-tests", Arity::Optional, "filter", true, LaunchMode::UnitTests, 0,
     [](LaunchOptions& o, const std::string& v) { o.unitTestsFilter = v; },
     "run the C++ unit tests (no window or GL); exit 1 on any failure"},
    {"--model-report", Arity::Many, "folder|model", true, LaunchMode::ModelReport, 0,
     [](LaunchOptions& o, const std::string& v) { o.inputs.push_back(v); },
     "print each model's size, bounds and mesh count"},
    {"--model-nodes", Arity::Many, "folder|fbx", true, LaunchMode::ModelNodes, 0,
     [](LaunchOptions& o, const std::string& v) { o.inputs.push_back(v); },
     "print each FBX's node tree"},
    {"--scene-bounds", Arity::Many, "scene.json|prefab dir", true, LaunchMode::SceneBounds, 0,
     [](LaunchOptions& o, const std::string& v) { o.inputs.push_back(v); },
     "write model bounds per scene/prefab as JSON (needs --out)"},
    {"--audio-test", Arity::Optional, "scene", true, LaunchMode::AudioTest, 0,
     [](LaunchOptions& o, const std::string& v) { o.audioTestScene = v; },
     "offline audio test: a scripted tour of a scene's reverb zones (default AudioLab)"},
    {"--crash-test", Arity::Required, "kind", true, LaunchMode::CrashTest, 0,
     [](LaunchOptions& o, const std::string& v) { o.crashTestKind = v; },
     "crash on purpose (av|overflow|abort|purecall|invalidparam|terminate) to check the crash handler"},
    {"--help", Arity::None, "", true, LaunchMode::Help, 0,
     [](LaunchOptions&, const std::string&) {},
     "this list"},

    // Modifiers.
    {"--project", Arity::Required, "dir", false, kNoMode, kAnyMode,
     [](LaunchOptions& o, const std::string& v) { o.projectDir = v; },
     "open this project folder instead of the one next to the exe"},
    {"--weapon-lab", Arity::None, "", false, kNoMode, kGuiModes,
     [](LaunchOptions& o, const std::string&) { o.weaponLabMode = true; },
     "the WeaponLab scene with the Weapon Lab panel"},
    {"--perf-sample", Arity::None, "", false, kNoMode, Bit(LaunchMode::PerfBench),
     [](LaunchOptions& o, const std::string&) { o.perfSample = true; },
     "add a sampling CPU profile of each phase"},
    {"--perf-res", Arity::Required, "WxH", false, kNoMode,
     Bit(LaunchMode::PerfBench) | Bit(LaunchMode::EditorShot) | Bit(LaunchMode::EditorTests),
     [](LaunchOptions& o, const std::string& v) {
#pragma warning(suppress : 4996)
         if (std::sscanf(v.c_str(), "%dx%d", &o.perfResW, &o.perfResH) != 2 || o.perfResW <= 0 || o.perfResH <= 0)
             o.perfResW = o.perfResH = -1; // reported after the parse
     },
     "window size, for repeatable runs"},
    {"--smoke-shots", Arity::Required, "dir", false, kNoMode,
     Bit(LaunchMode::SmokeTest) | Bit(LaunchMode::NpcTest) | Bit(LaunchMode::PerfBench),
     [](LaunchOptions& o, const std::string& v) { o.smokeShotsDir = v; },
     "save Scene-view PNGs of each scene"},
    {"--editor-shot-default-layout", Arity::None, "", false, kNoMode, Bit(LaunchMode::EditorShot),
     [](LaunchOptions& o, const std::string&) { o.editorShotDefaultLayout = true; },
     "start from the default dock layout"},
    {"--editor-shot-curves", Arity::None, "", false, kNoMode, Bit(LaunchMode::EditorShot),
     [](LaunchOptions& o, const std::string&) { o.editorShotCurves = true; },
     "capture the recoil curve widget alone"},
    {"--editor-shot-ide", Arity::None, "", false, kNoMode, Bit(LaunchMode::EditorShot),
     [](LaunchOptions& o, const std::string&) { o.editorShotIDE = true; },
     "one Script IDE capture"},
    {"--out", Arity::Required, "dir", false, kNoMode, Bit(LaunchMode::SceneBounds),
     [](LaunchOptions& o, const std::string& v) { o.sceneBoundsOut = v; },
     "output folder"},
    {"--report", Arity::Required, "file", false, kNoMode, Bit(LaunchMode::AudioTest),
     [](LaunchOptions& o, const std::string& v) { o.audioTestReport = v; },
     "write the report here"},
    {"--crash-test-in-editor", Arity::Required, "kind", false, kNoMode, Bit(LaunchMode::Editor),
     [](LaunchOptions& o, const std::string& v) { o.crashTestInEditor = v; },
     "crash a few seconds into the editor with the scene dirty (tests the recovery save)"},
    {"--crash-dialog", Arity::None, "", false, kNoMode, Bit(LaunchMode::CrashTest) | Bit(LaunchMode::Editor),
     [](LaunchOptions& o, const std::string&) { o.crashDialog = true; },
     "show the crash dialog on a crash test"},
};
// clang-format on

const FlagDef* Find(const std::string& name) {
    for (const FlagDef& f : kFlags)
        if (name == f.Name) return &f;
    return nullptr;
}

const char* ModeFlagName(LaunchMode m) {
    if (m == LaunchMode::Editor) return "the editor";
    for (const FlagDef& f : kFlags)
        if (f.IsMode && f.Mode == m) return f.Name;
    return "?";
}

// "--perf-bench, --editor-shot or the editor"
std::string ModeList(unsigned modes) {
    std::vector<std::string> names;
    for (unsigned m = 0; m <= (unsigned)LaunchMode::Help; ++m)
        if (modes & (1u << m)) names.push_back(ModeFlagName((LaunchMode)m));
    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
        out += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
    return out;
}

bool IsValue(const char* a) { return a[0] != '-'; }

} // namespace

bool ParseLaunchOptions(int argc, char** argv, LaunchOptions& out, std::string& error) {
    LaunchOptions o;
    const FlagDef* modeFlag = nullptr;
    std::vector<const FlagDef*> modifiers;
    std::vector<std::string> stray;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "/?") a = "--help";
        if (a.rfind("--", 0) != 0 || a.size() == 2) {
            stray.push_back(a);
            continue;
        }
        const FlagDef* f = Find(a);
        if (!f) {
            error = "unknown option '" + a + "'";
            return false;
        }
        if (f->IsMode) {
            if (modeFlag) {
                error = modeFlag == f ? "'" + a + "' is given twice"
                                      : "'" + std::string(modeFlag->Name) + "' and '" + a + "' can't run together; pick one";
                return false;
            }
            modeFlag = f;
        } else {
            modifiers.push_back(f);
        }
        switch (f->Takes) {
            case Arity::None: f->Set(o, {}); break;
            case Arity::Optional:
                f->Set(o, i + 1 < argc && IsValue(argv[i + 1]) ? argv[++i] : std::string());
                break;
            case Arity::Required:
                if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
                    error = "'" + a + "' needs a value: " + a + " <" + f->Value + ">";
                    return false;
                }
                f->Set(o, argv[++i]);
                break;
            case Arity::Many:
                while (i + 1 < argc && IsValue(argv[i + 1])) f->Set(o, argv[++i]);
                break;
        }
    }

    o.mode = modeFlag ? modeFlag->Mode : LaunchMode::Editor;
    if (o.mode == LaunchMode::Help) {
        out = o;
        return true;
    }
    for (const FlagDef* f : modifiers) {
        if (f->Modes & Bit(o.mode)) continue;
        error = "'" + std::string(f->Name) + "' only works with " + ModeList(f->Modes) + ", not with " +
                ModeFlagName(o.mode);
        return false;
    }
    if (o.perfResW < 0) {
        error = "'--perf-res' needs a size like 1920x1080";
        return false;
    }

    // Loose words after a list mode (--scene-bounds a.json --out d b.json) are more of its inputs; anywhere else
    // (a file dragged onto the exe) they're ignored.
    const bool takesList = modeFlag && modeFlag->Takes == Arity::Many;
    for (const std::string& s : stray) {
        if (takesList) o.inputs.push_back(s);
        else std::cerr << "Ignoring the argument '" << s << "'." << std::endl;
    }

    if (Env::Flag(Env::Var::WeaponLab)) o.weaponLabMode = true; // run-weapon-lab.cmd starts the exe without arguments

    o.smokeTestMode = o.mode == LaunchMode::SmokeTest || o.mode == LaunchMode::PerfBench || o.mode == LaunchMode::NpcTest;
    o.perfBenchMode = o.mode == LaunchMode::PerfBench;
    o.npcTestMode = o.mode == LaunchMode::NpcTest;
    o.editorTestsMode = o.mode == LaunchMode::EditorTests;
    o.buildMode = o.mode == LaunchMode::Build;

    if (!o.projectDir.empty()) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::absolute(o.projectDir, ec);
        if (ec || !std::filesystem::is_directory(dir, ec)) {
            error = "--project: '" + o.projectDir + "' is not a folder";
            return false;
        }
        ProjectPaths::SetRootOverride(dir.lexically_normal().string());
    }
    out = std::move(o);
    return true;
}

std::string LaunchHelpText() {
    const auto usage = [](const FlagDef& f) {
        std::string s = std::string("  ") + f.Name;
        switch (f.Takes) {
            case Arity::None: break;
            case Arity::Optional: s += std::string(" [") + f.Value + "]"; break;
            case Arity::Required: s += std::string(" <") + f.Value + ">"; break;
            case Arity::Many: s += std::string(" <") + f.Value + ">..."; break;
        }
        if (s.size() < 44) s.append(44 - s.size(), ' ');
        else s += ' ';
        return s;
    };
    std::string out = "Usage: TartarusEngine [mode] [options]\n\nModes (at most one; none opens the editor):\n";
    for (const FlagDef& f : kFlags)
        if (f.IsMode) out += usage(f) + f.Help + "\n";
    out += "\nOptions:\n";
    for (const FlagDef& f : kFlags) {
        if (f.IsMode) continue;
        out += usage(f) + f.Help;
        if (f.Modes != kAnyMode) out += " (with " + ModeList(f.Modes) + ")";
        out += "\n";
    }
    out += "\n" + Env::HelpText();
    return out;
}
