#include "UnitTestSupport.h"
#include "../App/LaunchOptions.h"
#include "../App/ScratchProfile.h"
#include "../Core/ModuleCopies.h"
#include "../Core/Env.h"
#include "../Core/Paths.h"

#include <string>
#include <vector>

namespace {

bool Parse(std::vector<std::string> args, LaunchOptions& o, std::string& err) {
    args.insert(args.begin(), "TartarusEngine.exe");
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    err.clear();
    return ParseLaunchOptions((int)argv.size(), argv.data(), o, err);
}

void Test_LaunchOptions_Modes() {
    LaunchOptions o;
    std::string err;
    CHECK(Parse({}, o, err) && o.mode == LaunchMode::Editor && !o.smokeTestMode);
    CHECK(Parse({"--perf-bench", "a.json", "--perf-res", "1280x720", "--perf-sample"}, o, err));
    CHECK(o.mode == LaunchMode::PerfBench && o.smokeTestMode && o.perfBenchMode && o.perfSample);
    CHECK(o.smokeScenesDirArg == "a.json" && o.perfResW == 1280 && o.perfResH == 720);
    CHECK(Parse({"--npc-test", "labloco", "--smoke-shots", "out"}, o, err));
    CHECK(o.mode == LaunchMode::NpcTest && o.smokeTestMode && o.npcTestScenario == "labloco" && o.smokeShotsDir == "out");
    CHECK(Parse({"--smoke-test", "--smoke-shots", "out"}, o, err) && o.smokeScenesDirArg.empty());
    CHECK(Parse({"--unit-tests", "Env"}, o, err) && o.mode == LaunchMode::UnitTests && o.unitTestsFilter == "Env");
    CHECK(Parse({"--help"}, o, err) && o.mode == LaunchMode::Help);
    CHECK(Parse({"-h"}, o, err) && o.mode == LaunchMode::Help);
    CHECK(Parse({"--import-blood", "pack"}, o, err) && o.mode == LaunchMode::ImportBlood && o.importDir == "pack");
    CHECK(Parse({"--import-knife", "packs"}, o, err) && o.mode == LaunchMode::ImportKnife && o.importDir == "packs");
    CHECK(!Parse({"--import-blood"}, o, err) && err.find("needs a value") != std::string::npos);
    // An empty value (a script whose path conversion failed) used to start a plain editor that never exits.
    CHECK(!Parse({"--editor-shot", ""}, o, err) && err.find("needs a value") != std::string::npos);
    // Rebuild & Restart's relaunch: editor only.
    CHECK(Parse({"--restore-state", "s.json", "--wait-pid", "4242"}, o, err));
    CHECK(o.mode == LaunchMode::Editor && o.restoreStateFile == "s.json" && o.waitPid == 4242);
    CHECK(!Parse({"--unit-tests", "--restore-state", "s.json"}, o, err));
}

void Test_LaunchOptions_Lists() {
    LaunchOptions o;
    std::string err;
    CHECK(Parse({"--scene-bounds", "a.json", "--out", "d", "b.json"}, o, err));
    CHECK(o.mode == LaunchMode::SceneBounds && o.sceneBoundsOut == "d" && o.inputs == std::vector<std::string>({"a.json", "b.json"}));
    CHECK(Parse({"--model-report", "x", "y"}, o, err) && o.inputs.size() == 2);
    CHECK(Parse({"--audio-test", "Lab", "--report", "r.txt"}, o, err) && o.audioTestScene == "Lab" && o.audioTestReport == "r.txt");
    // A loose word in editor mode (a file dropped on the exe) is ignored, not an error.
    CHECK(Parse({"dropped.json"}, o, err) && o.mode == LaunchMode::Editor && o.inputs.empty());
}

void Test_LaunchOptions_Errors() {
    LaunchOptions o;
    std::string err;
    // The old parser let a later --smoke-test silently turn perf-bench off.
    CHECK(!Parse({"--perf-bench", "x", "--smoke-test"}, o, err) && err.find("can't run together") != std::string::npos);
    CHECK(!Parse({"--smoke-test", "--smoke-test"}, o, err));
    CHECK(!Parse({"--perf-bnech"}, o, err) && err.find("unknown option") != std::string::npos);
    CHECK(!Parse({"--smoke-test", "--smoke-shots"}, o, err) && err.find("needs a value") != std::string::npos);
    CHECK(!Parse({"--editor-shot", "--editor-shot-ide"}, o, err));
    CHECK(!Parse({"--smoke-test", "--perf-sample"}, o, err) && err.find("only works with") != std::string::npos);
    CHECK(!Parse({"--perf-res", "1280x720"}, o, err)); // the editor doesn't take it
    CHECK(!Parse({"--perf-bench", "--perf-res", "big"}, o, err) && err.find("1920x1080") != std::string::npos);
    for (const char* pid : {"abc", "0", "12x", "+12", " 12", "99999999999", "4294967295"})
        CHECK(!Parse({"--wait-pid", pid}, o, err) && err.find("process id") != std::string::npos);
    CHECK(!Parse({"--wait-pid", "-5"}, o, err)); // a value starting with '-' is a missing value
    CHECK(Parse({"--wait-pid", "4294967294"}, o, err) && o.waitPid == 4294967294ul);
    CHECK(!Parse({"--project", "Z:/no/such/folder/anywhere"}, o, err));
}

void Test_Env_ReadsAndHelp() {
    const Env::Var v = Env::Var::TrisReport;
    const bool wasSet = Env::IsSet(v);
    const std::string was = Env::Str(v);
    Env::Set(v, nullptr);
    CHECK(!Env::IsSet(v) && !Env::Flag(v) && Env::Int(v, -1) == -1 && Env::Str(v, "d") == "d");
    Env::Set(v, "0");
    CHECK(Env::IsSet(v) && !Env::Flag(v) && Env::Int(v, -1) == 0);
    Env::Set(v, "42");
    CHECK(Env::Flag(v) && Env::Int(v, -1) == 42 && Env::Float(v, 0.0f) == 42.0f);
    Env::Set(v, "abc");
    CHECK(Env::Flag(v) && Env::Int(v, 7) == 7);
    Env::Set(v, wasSet ? was.c_str() : nullptr);

    const std::string help = LaunchHelpText();
    for (int i = 0; i < (int)Env::Var::Count; ++i)
        CHECK(help.find(Env::Describe((Env::Var)i).Name) != std::string::npos);
    CHECK(help.find("--perf-bench") != std::string::npos && help.find("--crash-dialog") != std::string::npos);
    CHECK(help.find("--import-blood") != std::string::npos && help.find("--import-knife") != std::string::npos);
}

// The text-only answer Relativize gives for a path spelled under the project root: no disk access,
// so the Asset Browser can file thousands of not-yet-fetched paths in one frame.
void Test_ProjectPaths_RelativizeUnder() {
    std::string r;
    const std::string root = "C:\\Proj\\project";
    CHECK(ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\assets\\a.png", r) && r == "assets/a.png");
    CHECK(ProjectPaths::RelativizeUnder(root, "c:/proj/PROJECT/assets/Sub/B.fbx", r) && r == "assets/Sub/B.fbx"); // drive letter + root case
    CHECK(ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project/assets\\x/y.wav", r) && r == "assets/x/y.wav");    // mixed slashes
    CHECK(ProjectPaths::RelativizeUnder(root + "\\", "C:\\Proj\\project\\a.json", r) && r == "a.json");            // root with a trailing slash
    CHECK(ProjectPaths::RelativizeUnder("C:/Proj/project/", "C:\\Proj\\project\\a.json", r) && r == "a.json");
    r = "unchanged";
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project", r) && r == "unchanged");         // the root itself
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\", r));
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\assets\\", r));                   // trailing slash: the disk decides
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\projectX\\a.png", r));                     // a sibling sharing the prefix
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\other\\a.png", r));                        // outside
    CHECK(!ProjectPaths::RelativizeUnder(root, "D:\\Proj\\project\\a.png", r));                      // another drive
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\..\\secret.txt", r));             // ".." walks out
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\a\\..\\b.png", r));
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\.\\b.png", r));
    CHECK(!ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\a\\\\b.png", r));                 // empty segment
    CHECK(ProjectPaths::RelativizeUnder(root, "C:\\Proj\\project\\.hidden\\a..b.png", r) && r == ".hidden/a..b.png"); // dots inside names are fine
    CHECK(!ProjectPaths::RelativizeUnder("", "C:\\a.png", r));
    // Relativize itself: under the real root, the same answer as the text; outside, the input back.
    const std::string& real = ProjectPaths::Root();
    CHECK(ProjectPaths::Relativize(real + "\\assets\\NotOnDisk\\x.png") == "assets/NotOnDisk/x.png");
    CHECK(ProjectPaths::Relativize("Z:\\elsewhere\\x.png") == "Z:\\elsewhere\\x.png");
}

void Test_ScratchProfile_InstanceNames() {
    // TARTARUS_EDITOR_TEST_INSTANCE: parallel harness runs get their own user folder; no name = the old folder.
    CHECK(ScratchFolderName("TartarusEngine-Test", "") == "TartarusEngine-Test");
    CHECK(ScratchFolderName("TartarusEngine-Shot", "w1") == "TartarusEngine-Shot-w1");
    CHECK(ScratchInstanceNameValid("") && ScratchInstanceNameValid("w1") && ScratchInstanceNameValid("bench_2-b"));
    CHECK(!ScratchInstanceNameValid("a b") && !ScratchInstanceNameValid("..") && !ScratchInstanceNameValid("x/y"));
    CHECK(!ScratchInstanceNameValid("w1\\..") && !ScratchInstanceNameValid(std::string(33, 'a')));
}

void Test_ModuleCopies_OwnerPid() {
    // The hot-reload sweep keeps copies a running instance owns; it reads the pid from the name.
    CHECK(ModuleCopies::OwnerPid(L"TartarusEditor_1234_1.dll", L"TartarusEditor_") == 1234u);
    CHECK(ModuleCopies::OwnerPid(L"TartarusGame_7_12.dll", L"TartarusGame_") == 7u);
    CHECK(!ModuleCopies::OwnerPid(L"TartarusGame_3.dll", L"TartarusGame_"));       // old style: no pid
    CHECK(!ModuleCopies::OwnerPid(L"TartarusGame_x_1.dll", L"TartarusGame_"));
    CHECK(!ModuleCopies::OwnerPid(L"TartarusEditor_5_1.dll", L"TartarusGame_"));
    CHECK(!ModuleCopies::OwnerPid(L"TartarusGame_99999999999_1.dll", L"TartarusGame_"));
}

} // namespace

void RegisterLaunchOptionsTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"LaunchOptions::Modes", Test_LaunchOptions_Modes});
    tests.push_back({"LaunchOptions::Lists", Test_LaunchOptions_Lists});
    tests.push_back({"LaunchOptions::Errors", Test_LaunchOptions_Errors});
    tests.push_back({"Env::ReadsAndHelp", Test_Env_ReadsAndHelp});
    tests.push_back({"ProjectPaths::RelativizeUnder", Test_ProjectPaths_RelativizeUnder});
    tests.push_back({"ScratchProfile::InstanceNames", Test_ScratchProfile_InstanceNames});
    tests.push_back({"ModuleCopies::OwnerPid", Test_ModuleCopies_OwnerPid});
}
