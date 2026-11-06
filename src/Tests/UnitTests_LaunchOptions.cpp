#include "UnitTestSupport.h"
#include "../App/LaunchOptions.h"
#include "../Core/Env.h"

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
    CHECK(Parse({"--npc-test", "fight", "--smoke-shots", "out"}, o, err));
    CHECK(o.mode == LaunchMode::NpcTest && o.smokeTestMode && o.npcTestScenario == "fight" && o.smokeShotsDir == "out");
    CHECK(Parse({"--smoke-test", "--smoke-shots", "out"}, o, err) && o.smokeScenesDirArg.empty());
    CHECK(Parse({"--unit-tests", "Env"}, o, err) && o.mode == LaunchMode::UnitTests && o.unitTestsFilter == "Env");
    CHECK(Parse({"--help"}, o, err) && o.mode == LaunchMode::Help);
    CHECK(Parse({"-h"}, o, err) && o.mode == LaunchMode::Help);
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
}

} // namespace

void RegisterLaunchOptionsTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"LaunchOptions::Modes", Test_LaunchOptions_Modes});
    tests.push_back({"LaunchOptions::Lists", Test_LaunchOptions_Lists});
    tests.push_back({"LaunchOptions::Errors", Test_LaunchOptions_Errors});
    tests.push_back({"Env::ReadsAndHelp", Test_Env_ReadsAndHelp});
}
