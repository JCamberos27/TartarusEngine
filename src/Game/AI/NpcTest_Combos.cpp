// --npc-test combos: the weapon combo matrix (project/scenes/AnimTestbed.json, the open ground behind the lane). Every cell is
// one (weapon, action, movement state): it starts from the fresh body (FirstPersonBody Stop + Start) placed at the same spot,
// settles with the weapon ready, plays the state's lead-in, then the action, and measures every frame of the lead-in and the
// action with both jank meters - FirstPersonJankMeter (what the player sees: the hands in the camera's space, the grip gap,
// the reach, the arm in the eye) and WeaponJankMeter (the world body's gun and hands, the third-person clips' own motion
// taken out). Only events from the action's start count. One "[Combo] ROW" line a cell: PASS (no event) or FLAG with the
// worst event (scored against its meter's threshold), the event counts, the gait cycle's largest hand bounce, the controller
// states the action went through. tools/combo_matrix.py turns the log into a table, worst first.
//   weapons  ak, 870, rev (TARTARUS_COMBOS, comma-separated; default ak)
//   actions  hold (nothing: the state's own control row), draw, holster, swap_<other>, aim_in, aim_out, fire, fire_hold (the
//            AK, full auto), reload_tac (half a magazine), reload_empty, inspect, magcheck, melee
//   states   idle, walk, jog, sprint, crouch_idle, crouch_walk, turn (in place, 90 deg/s), jump (pressed as the action starts),
//            and a change 0.3 s into the action - idle>walk, walk>idle, idle>crouch - for reload_tac, aim_in and the first swap
// TARTARUS_COMBOS_CELLS=<patterns> runs only the cells whose id (weapon/action/state, e.g. ak/reload_tac/walk) matches one of
// the comma-separated patterns (* wildcards). TARTARUS_COMBOS_CLIPS=<dir>: each flagged cell is played again at once from the
// same fresh body, recorded every frame into <dir>/<cell>/ (game_ = the player's view, scene_ = the world body from its front
// right). A cell flagged only by the walking bounce gets its clip when the bounce is over 1.3x its state's hold row (that row
// itself gets one).
#include "NpcTest.h"
#include "Core/Env.h"
#include "BodyDebugDraw.h"
#include "Camera.h"
#include "FirstPersonBody.h"
#include "FirstPersonJankMeter.h"
#include "FirstPersonPresentation.h"
#include "Npc.h"
#include "NpcDirector.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "WeaponJankMeter.h"
#include "WeaponLab.h" // WeaponJankFrameFor
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

namespace {
enum class CbAct : unsigned char { Hold, Draw, Holster, Swap, AimIn, AimOut, Fire, FireHold, ReloadTac, ReloadEmpty, Inspect, MagCheck, Melee };
enum class CbMove : unsigned char { Idle, Walk, Jog, Sprint, CrouchIdle, CrouchWalk, Turn, Jump, IdleToWalk, WalkToIdle, IdleToCrouch };
struct CbStateDef {
    const char* Name;
    CbMove Move;
    float Lead; // s of the state before the action starts
};
const CbStateDef kCbStates[] = {
    {"idle", CbMove::Idle, 0.5f},          {"walk", CbMove::Walk, 1.2f},
    {"jog", CbMove::Jog, 1.2f},            {"sprint", CbMove::Sprint, 1.5f},
    {"crouch_idle", CbMove::CrouchIdle, 1.0f}, {"crouch_walk", CbMove::CrouchWalk, 1.4f},
    {"turn", CbMove::Turn, 0.6f},          {"jump", CbMove::Jump, 0.4f},
    {"idle>walk", CbMove::IdleToWalk, 0.5f}, {"walk>idle", CbMove::WalkToIdle, 1.2f},
    {"idle>crouch", CbMove::IdleToCrouch, 0.5f},
};
constexpr int kCbSteadyStates = 8; // the first eight: every action; the rest (a change mid-action) for a few
constexpr float kCbChangeAt = 0.3f; // s into the action the mid-action change comes
struct CbWeapon {
    const char* Name;
    int Key;       // FirstPersonPresentation::SlotForKey: 0 primary, 1 secondary, 2 sidearm
    bool FullAuto; // fire_hold applies
};
const CbWeapon kCbWeapons[] = {{"ak", 0, true}, {"870", 1, false}, {"rev", 2, false}};
struct CbActDef {
    const char* Name;
    CbAct Act;
    float Fixed; // > 0: the window is this long; else until the controller is back at rest ...
    float Cap;   // ... at most this long
};
const CbActDef kCbActs[] = {
    {"hold", CbAct::Hold, 2.0f, 2.0f},          {"draw", CbAct::Draw, 0.0f, 3.0f},
    {"holster", CbAct::Holster, 0.0f, 3.0f},    {"swap", CbAct::Swap, 0.0f, 5.0f},
    {"aim_in", CbAct::AimIn, 1.5f, 1.5f},       {"aim_out", CbAct::AimOut, 1.5f, 1.5f},
    {"fire", CbAct::Fire, 1.5f, 1.5f},          {"fire_hold", CbAct::FireHold, 2.2f, 2.2f},
    {"reload_tac", CbAct::ReloadTac, 0.0f, 7.0f}, {"reload_empty", CbAct::ReloadEmpty, 0.0f, 8.0f},
    {"inspect", CbAct::Inspect, 0.0f, 8.0f},    {"magcheck", CbAct::MagCheck, 0.0f, 5.0f},
    {"melee", CbAct::Melee, 0.0f, 3.0f},
};
constexpr float kCbSettleMin = 1.0f, kCbSettleCap = 6.0f;
constexpr float kCbFireHoldFor = 1.2f; // s the trigger is held on fire_hold
// The cells start here, facing +X (yaw 0): 54 m of clear ground ahead (the 60 m ground, the lane's posts and dummies are at z > 0).
const glm::vec3 kCbStart(-27.0f, 0.05f, -24.0f);
constexpr float kCbYaw = 0.0f;

struct CbCell {
    int Weapon = 0;
    int Act = 0;       // kCbActs
    int SwapTo = -1;   // kCbWeapons (swap)
    int State = 0;     // kCbStates
    std::string Id;
    bool Replay = false;
    float MaxSeconds() const { return kCbSettleCap + std::max(kCbStates[State].Lead, 1.0f) + kCbActs[Act].Cap + 0.2f; }
};

bool CbMatch(const std::string& s, const std::string& p) {
    size_t i = 0, j = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (j < p.size() && (p[j] == s[i])) { ++i; ++j; }
        else if (j < p.size() && p[j] == '*') { star = j++; mark = i; }
        else if (star != std::string::npos) { j = star + 1; i = ++mark; }
        else return false;
    }
    while (j < p.size() && p[j] == '*') ++j;
    return j == p.size();
}
std::vector<std::string> CbList(const std::string& s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t end = std::min(s.find(',', at), s.size());
        std::string part = s.substr(at, end - at);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) out.push_back(part);
        at = end + 1;
    }
    return out;
}

std::vector<CbCell> CbPlan() {
    std::vector<CbCell> cells;
    std::vector<std::string> weapons = CbList(Env::IsSet(Env::Var::Combos) ? Env::Str(Env::Var::Combos) : std::string("ak"));
    if (weapons.empty()) weapons.push_back("ak");
    const std::vector<std::string> only = CbList(Env::IsSet(Env::Var::CombosCells) ? Env::Str(Env::Var::CombosCells) : std::string());
    auto add = [&](int w, int a, int swapTo, int st) {
        CbCell c;
        c.Weapon = w;
        c.Act = a;
        c.SwapTo = swapTo;
        c.State = st;
        const std::string act = swapTo >= 0 ? std::string("swap_") + kCbWeapons[swapTo].Name : kCbActs[a].Name;
        c.Id = std::string(kCbWeapons[w].Name) + "/" + act + "/" + kCbStates[st].Name;
        if (!only.empty() && std::none_of(only.begin(), only.end(), [&](const std::string& p) { return CbMatch(c.Id, p); })) return;
        cells.push_back(c);
    };
    for (const std::string& name : weapons) {
        int w = -1;
        for (int k = 0; k < 3; ++k)
            if (name == kCbWeapons[k].Name) w = k;
        if (w < 0) {
            std::printf("[Combo] no weapon '%s' (ak, 870, rev)\n", name.c_str());
            continue;
        }
        // State by state, the hold row first (the bounce the other rows are compared with).
        for (int st = 0; st < (int)(sizeof kCbStates / sizeof kCbStates[0]); ++st) {
            const bool steady = st < kCbSteadyStates;
            for (int a = 0; a < (int)(sizeof kCbActs / sizeof kCbActs[0]); ++a) {
                const CbAct act = kCbActs[a].Act;
                if (act == CbAct::FireHold && !kCbWeapons[w].FullAuto) continue;
                if (act == CbAct::Swap) {
                    bool first = true;
                    for (int o = 0; o < 3; ++o) {
                        if (o == w) continue;
                        if (steady || first) add(w, a, o, st);
                        first = false;
                    }
                    continue;
                }
                if (steady || act == CbAct::ReloadTac || act == CbAct::AimIn) add(w, a, -1, st);
            }
        }
    }
    return cells;
}

bool CbResting(const std::string& s) {
    return s == "Idle" || s == "Walk" || s == "Sprint" || s == "Aim" || s == "IdleToSprint" || s == "SprintToIdle";
}
std::string CbDirName(std::string id) {
    for (char& c : id)
        if (c == '/' || c == '>') c = c == '/' ? '-' : '~';
    return id;
}
} // namespace

struct NpcTest::CombosRun {
    std::vector<CbCell> Cells;
    size_t At = 0;
    enum class Phase { Settle, Lead, Window } Ph = Phase::Settle;
    float PhaseT = 0.0f;   // s in this phase
    bool Entered = false;  // this cell's settle has begun
    bool SlotAsked = false, Refilled = false, ModeSet = false;
    float RestFor = 0.0f;  // s the controller has sat in a resting state
    float TurnT = 0.0f;    // s the view has turned (turn: from the lead-in's start)
    bool LeftRest = false; // ... and the action has taken it out of one
    bool Jumped = false;
    bool SetupFailed = false;
    std::string States;    // the controller states the window went through
    std::string LastState;
    int AmmoAt = -1;       // the magazine as the action started (after a reload's own SetAmmo)
    // The meters (fed in Pose; reset as the lead-in starts, events counted from the action's start).
    FirstPersonJankMeter Fp;
    WeaponJankMeter Tp;
    bool MeterOn = false, ResetMeters = false, MarkWindow = false;
    float FpFrom = 0.0f, TpFrom = 0.0f;
    float CycleMm = -1.0f, LastCycleSeen = -1.0f;
    // The support (left) arm through the action, whatever the thresholds: its largest grip gap first person (body hand off
    // the rig) and third person (world hand off the world gun), and its largest reach (of the arm's length).
    float SupGripFp = 0.0f, SupGripTp = 0.0f, SupReach = 0.0f;
    std::map<std::string, float> HoldBounce; // weapon/state -> the hold row's bounce (mm)
    int Rows = 0, Flagged = 0, Setups = 0, Replays = 0;
    std::string ClipsDir;
    // The Scene view, this frame.
    glm::vec3 Feet{0.0f};
};

float NpcTest::CombosSeconds() {
    float t = 0.0f;
    for (const CbCell& c : CbPlan()) t += c.MaxSeconds();
    return t + 2.0f;
}

void NpcTest::CombosDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    if (!m_Combo) {
        m_Combo = std::make_shared<CombosRun>();
        m_Combo->Cells = CbPlan();
        m_Combo->ClipsDir = Env::IsSet(Env::Var::CombosClips) ? Env::Str(Env::Var::CombosClips) : std::string();
        std::printf("[Combo] %zu cells%s\n", m_Combo->Cells.size(), m_Combo->ClipsDir.empty() ? "" : " (flagged cells recorded)");
    }
    CombosRun& R = *m_Combo;
    m_Action.clear();
    m_Reload = false;
    m_Firing = false;
    m_Target.clear();
    BodyDebug::ProbeRequested() = true; // the camera probe: the eye-to-arm distances and the rays into the body
    wantYaw = player.Cam.Yaw = kCbYaw;
    wantPitch = -3.0f;
    player.ScriptMove = glm::vec2(0.0f);
    player.ScriptSprint = player.ScriptRun = player.ScriptWalk = player.ScriptCrouch = false;
    if (R.At >= R.Cells.size()) {
        if (!m_Done) {
            Check(R.Rows > 0, "combos: the matrix ran (" + std::to_string(R.Rows) + " cells, " + std::to_string(R.Flagged) + " flagged, " +
                                  std::to_string(R.Replays) + " recorded)");
            Check(R.Setups == 0, "combos: every cell got its weapon ready in " + std::to_string((int)kCbSettleCap) + " s (" +
                                     std::to_string(R.Setups) + " didn't)");
            m_Done = true;
        }
        return;
    }
    const CbCell& C = R.Cells[R.At];
    const CbStateDef& S = kCbStates[C.State];
    const CbActDef& A = kCbActs[C.Act];
    const int slot = weapon.SlotForKey(kCbWeapons[C.Weapon].Key);
    const int swapSlot = C.SwapTo >= 0 ? weapon.SlotForKey(kCbWeapons[C.SwapTo].Key) : -1;
    const std::string state = weapon.CurrentState();
    R.PhaseT += dt;

    if (!R.Entered) { // the fresh body, placed and standing
        R.Entered = true;
        R.Ph = CombosRun::Phase::Settle;
        R.PhaseT = 0.0f;
        R.SlotAsked = R.Refilled = R.ModeSet = R.LeftRest = R.Jumped = R.SetupFailed = false;
        R.RestFor = R.TurnT = 0.0f;
        R.States.clear();
        R.MeterOn = false;
        m_BodyReset = true;
        PhysicsWorld::SetCharacterFootPosition(&kCbStart.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kCbStart + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        std::printf("[Combo] %7.2f s  %s%s\n", m_FireHold, C.Id.c_str(), C.Replay ? " (recorded)" : "");
    }
    m_FireHold += dt; // (the scenario's clock, for the log)

    auto finishCell = [&](bool setupFailed) {
        const bool clips = !R.ClipsDir.empty();
        if (C.Replay) {
            m_RecordDir.clear();
            std::printf("[Combo] REPLAY %s|fp=%zu|tp=%zu\n", C.Id.c_str(), R.Fp.Events().size(), R.Tp.Events().size());
            ++R.At;
            R.Entered = false;
            return;
        }
        ++R.Rows;
        if (setupFailed) {
            ++R.Setups;
            std::printf("[Combo] ROW %s|SETUP|0|the weapon wasn't ready (slot %d, state %s)|fp=0|tp=0|bounce=-|-|-\n", C.Id.c_str(),
                        weapon.Slot(), state.c_str());
            ++R.At;
            R.Entered = false;
            return;
        }
        // Score every event against its meter's threshold; the worst names the row.
        float worst = 0.0f;
        std::string worstText = "-";
        bool nonBounce = false;
        int fpN = 0, tpN = 0;
        std::vector<std::string> lines;
        for (const FirstPersonJankEvent& e : R.Fp.Events()) {
            if (e.Time < R.FpFrom) continue;
            // A bounce is the gait's, in a held state: a reload's or an inspect's own hand travel over a cycle isn't one.
            if (e.What == FirstPersonJankEvent::Kind::Bounce && !CbResting(e.State)) continue;
            ++fpN;
            float s = 0.0f;
            switch (e.What) {
            case FirstPersonJankEvent::Kind::Bounce: s = e.Size / R.Fp.BounceMm; break;
            case FirstPersonJankEvent::Kind::HandPop: s = e.Size / R.Fp.PopCm; nonBounce = true; break;
            case FirstPersonJankEvent::Kind::Grip: s = e.Size / R.Fp.GripCm; nonBounce = true; break;
            case FirstPersonJankEvent::Kind::Stretch: s = 1.0f + e.Size / 10.0f; nonBounce = true; break;
            case FirstPersonJankEvent::Kind::Clip: s = 1.0f + std::max(0.0f, R.Fp.ClipCm - e.Size) / R.Fp.ClipCm; nonBounce = true; break;
            }
            char at[32];
            std::snprintf(at, sizeof at, " (+%.2f s)", e.Time - R.FpFrom);
            lines.push_back("FP " + e.Describe() + at);
            if (s > worst) { worst = s; worstText = "FP " + std::string(FirstPersonJankEvent::KindName(e.What)) + (e.Side == 0 ? " L " : e.Side == 1 ? " R " : " ");
                             char v[64]; std::snprintf(v, sizeof v, "%.2f %s in %s +%.2f s", e.Size, e.What == FirstPersonJankEvent::Kind::Bounce ? "mm" :
                                 e.What == FirstPersonJankEvent::Kind::Stretch ? "%" : "cm", e.State.c_str(), e.Time - R.FpFrom);
                             worstText += v; }
        }
        for (const WeaponJankEvent& e : R.Tp.Events()) {
            if (e.Time < R.TpFrom) continue;
            ++tpN;
            nonBounce = true;
            float s = 0.0f;
            switch (e.What) {
            case WeaponJankEvent::Kind::GunPop: s = e.Size / (e.Turn ? R.Tp.PopDeg : R.Tp.PopCm); break;
            case WeaponJankEvent::Kind::HandPop: s = e.Size / R.Tp.PopCm; break;
            case WeaponJankEvent::Kind::Settle: s = e.Size / (e.Turn ? R.Tp.SettleMinDeg : R.Tp.SettleMinCm); break;
            case WeaponJankEvent::Kind::Grip: s = e.Size / R.Tp.GripCm; break;
            }
            char at[32];
            std::snprintf(at, sizeof at, " (+%.2f s)", e.Time - R.TpFrom);
            lines.push_back("TP " + e.Describe() + at);
            if (s > worst) { worst = s; worstText = "TP " + std::string(WeaponJankEvent::KindName(e.What)) + (e.Side == 0 ? " L " : e.Side == 1 ? " R " : " ");
                             char v[64]; std::snprintf(v, sizeof v, "%.2f %s in %s +%.2f s", e.Size, e.Turn ? "deg" : "cm", e.State.c_str(), e.Time - R.TpFrom);
                             worstText += v; }
        }
        const bool flagged = fpN + tpN > 0;
        const std::string key = std::string(kCbWeapons[C.Weapon].Name) + "/" + S.Name;
        const bool holdRow = A.Act == CbAct::Hold;
        if (holdRow) R.HoldBounce[key] = R.CycleMm;
        const auto hb = R.HoldBounce.find(key);
        const bool bounceOver = hb == R.HoldBounce.end() || hb->second <= 0.0f || R.CycleMm > 1.3f * hb->second;
        const bool clip = clips && flagged && (nonBounce || holdRow || bounceOver);
        std::string clipPath = "-";
        if (clip) clipPath = (std::filesystem::path(R.ClipsDir) / CbDirName(C.Id)).string();
        R.Flagged += flagged;
        char bounce[16];
        if (R.CycleMm >= 0.0f) std::snprintf(bounce, sizeof bounce, "%.1f", R.CycleMm);
        else std::snprintf(bounce, sizeof bounce, "-");
        char ammo[48]; // the rounds fired, the magazine refilled: the action really ran
        std::snprintf(ammo, sizeof ammo, " [ammo %d->%d]", R.AmmoAt, weapon.Ammo());
        std::printf("[Combo] ROW %s|%s|%.2f|%s|fp=%d|tp=%d|bounce=%s|%s%s|%s\n", C.Id.c_str(), flagged ? "FLAG" : "PASS", worst, worstText.c_str(),
                    fpN, tpN, bounce, R.States.empty() ? "-" : R.States.c_str(), ammo, clipPath.c_str());
        std::printf("[Combo] SUPPORT %s|grip_fp=%.2f|grip_tp=%.2f|reach=%.3f\n", C.Id.c_str(), R.SupGripFp * 100.0f, R.SupGripTp * 100.0f, R.SupReach);
        for (size_t k = 0; k < lines.size() && k < 16; ++k) std::printf("[Combo] EV %s|%s\n", C.Id.c_str(), lines[k].c_str());
        if (lines.size() > 16) std::printf("[Combo] EV %s|... %zu more\n", C.Id.c_str(), lines.size() - 16);
        if (clip) { // again at once, from the same fresh body, recorded
            CbCell again = C;
            again.Replay = true;
            R.Cells.insert(R.Cells.begin() + (std::ptrdiff_t)R.At + 1, again);
            m_Duration += again.MaxSeconds();
            ++R.Replays;
        }
        ++R.At;
        R.Entered = false;
    };

    // --- the settle: the weapon ready (the slot, drawn or holstered, a full magazine), the body standing -------------
    if (R.Ph == CombosRun::Phase::Settle) {
        if (slot < 0 || (C.SwapTo >= 0 && swapSlot < 0)) { finishCell(true); return; }
        if (weapon.Slot() != slot) {
            if (!R.SlotAsked) { weapon.SelectSlot(slot); R.SlotAsked = true; }
        } else if (A.Act == CbAct::Draw) {
            if (weapon.IsEquipped()) weapon.SetEquipped(false);
        } else if (!weapon.IsEquipped()) {
            weapon.SetEquipped(true);
        }
        if (weapon.Slot() == slot && !R.Refilled) { weapon.RefillAmmo(); R.Refilled = true; }
        if (weapon.Slot() == slot && A.Act == CbAct::FireHold && !weapon.IsFullAuto() && !R.ModeSet) { weapon.ToggleFireMode(); R.ModeSet = true; }
        const bool ready = weapon.Slot() == slot && (A.Act == CbAct::Draw ? state == "Holstered" : weapon.IsEquipped() && CbResting(state));
        R.RestFor = ready ? R.RestFor + dt : 0.0f;
        if (R.PhaseT >= kCbSettleMin && R.RestFor >= 0.3f) {
            R.Ph = CombosRun::Phase::Lead;
            R.PhaseT = 0.0f;
            R.MeterOn = true;
            R.ResetMeters = true;
            if (C.Replay && !R.ClipsDir.empty()) m_RecordDir = (std::filesystem::path(R.ClipsDir) / CbDirName(C.Id)).string();
        } else if (R.PhaseT >= kCbSettleCap) {
            finishCell(true);
        }
        return;
    }

    // --- the state: its moves through the lead-in and the action ---------------------------------------------------
    const bool inWindow = R.Ph == CombosRun::Phase::Window;
    const float since = inWindow ? R.PhaseT : -1.0f; // s into the action
    const bool changed = inWindow && since >= kCbChangeAt;
    glm::vec2 move(0.0f);
    switch (S.Move) {
    case CbMove::Walk: case CbMove::Jog: case CbMove::Sprint: case CbMove::CrouchWalk: move = {0.0f, 1.0f}; break;
    case CbMove::IdleToWalk: if (changed) move = {0.0f, 1.0f}; break;
    case CbMove::WalkToIdle: if (!changed) move = {0.0f, 1.0f}; break;
    default: break;
    }
    player.ScriptMove = move;
    player.ScriptSprint = S.Move == CbMove::Jog || S.Move == CbMove::Sprint;
    player.ScriptRun = S.Move == CbMove::Sprint;
    player.ScriptCrouch = S.Move == CbMove::CrouchIdle || S.Move == CbMove::CrouchWalk || (S.Move == CbMove::IdleToCrouch && changed);
    if (S.Move == CbMove::Turn) {
        R.TurnT += dt;
        wantYaw = player.Cam.Yaw = kCbYaw + 90.0f * R.TurnT;
    }
    if (S.Move == CbMove::Jump && inWindow && !R.Jumped) { player.ScriptJump = true; R.Jumped = true; }

    // --- the lead-in: aim_out holds the sights up through it ------------------------------------------------------------
    if (!inWindow) {
        if (A.Act == CbAct::AimOut) m_Target = "combos";
        const float lead = A.Act == CbAct::AimOut ? std::max(S.Lead, 1.0f) : S.Lead;
        if (R.PhaseT >= lead) { // the action starts this frame
            R.Ph = CombosRun::Phase::Window;
            R.PhaseT = 0.0f;
            R.MarkWindow = true;
            R.RestFor = 0.0f;
            R.States = R.LastState = state;
            switch (A.Act) {
            case CbAct::Draw: weapon.SetEquipped(true); break;
            case CbAct::Holster: weapon.SetEquipped(false); break;
            case CbAct::Swap: weapon.SelectSlot(swapSlot); break;
            case CbAct::AimIn: m_Target = "combos"; break;
            case CbAct::Fire: case CbAct::FireHold: m_Firing = true; break;
            case CbAct::ReloadTac: weapon.SetAmmo(std::max(1, weapon.MagazineSize() / 2)); m_Reload = true; break;
            case CbAct::ReloadEmpty: weapon.SetAmmo(0); m_Reload = true; break;
            case CbAct::Inspect: m_Action = "Inspect"; break;
            case CbAct::MagCheck: m_Action = "MagCheck"; break;
            case CbAct::Melee: m_Action = "Melee"; break;
            default: break;
            }
            R.AmmoAt = weapon.Ammo();
        }
        return;
    }

    // --- the action's window -----------------------------------------------------------------------------------------------
    if (A.Act == CbAct::AimIn) m_Target = "combos";
    if (A.Act == CbAct::FireHold && since < kCbFireHoldFor) m_Firing = true;
    if (state != R.LastState) {
        R.LastState = state;
        if (R.States.size() < 200) R.States += ">" + state;
    }
    bool rest = CbResting(state);
    if (A.Act == CbAct::Holster) rest = state == "Holstered";
    if (A.Act == CbAct::Swap) rest = rest && weapon.Slot() == swapSlot;
    if (!rest) R.LeftRest = true;
    R.RestFor = rest ? R.RestFor + dt : 0.0f;
    const bool done = A.Fixed > 0.0f ? since >= A.Fixed
                                     : since >= A.Cap || (since >= 0.6f && R.RestFor >= 0.4f && (R.LeftRest || since >= 1.2f));
    if (done) {
        if (A.Fixed <= 0.0f && since >= A.Cap) R.States += " (cap)";
        R.MeterOn = false;
        finishCell(false);
    }
}

void NpcTest::CombosPose(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p, const Camera& cam, float dt) {
    (void)cam;
    if (!m_Combo || dt <= 0.0f || !p.IsActive()) return;
    CombosRun& R = *m_Combo;
    if (R.ResetMeters) {
        R.Fp.Reset();
        R.Tp.Reset();
        R.ResetMeters = false;
        R.CycleMm = R.LastCycleSeen = -1.0f;
    }
    if (!R.MeterOn) return;
    if (R.MarkWindow) { // the action starts: events from here count
        R.FpFrom = R.Fp.Time();
        R.TpFrom = R.Tp.Time();
        R.MarkWindow = false;
        R.CycleMm = -1.0f;
        R.LastCycleSeen = R.Fp.LastCycleMm();
        R.SupGripFp = R.SupGripTp = R.SupReach = 0.0f;
    }
    const BodyDebug::Snapshot& bd = BodyDebug::Info();
    FirstPersonJankSample s;
    s.Dt = dt;
    s.State = p.CurrentState();
    glm::vec3 h[2];
    const bool hands = p.ArmsNodeInView("hand_l", h[0]) && p.ArmsNodeInView("hand_r", h[1]);
    s.Valid = hands && s.State != "Holstered";
    s.Recoil = p.SinceShot() < 0.35f || s.State == "Fire";
    s.Speed = p.PlanarSpeed();
    s.GaitPhase = p.HaveGaitPhase() ? p.GaitPhaseValue() : -1.0f;
    for (int k = 0; k < 2; ++k) {
        s.Hand[k] = glm::vec3(h[k].x, h[k].y, -h[k].z); // (the view looks down -Z)
        s.HandGap[k] = bd.HandGap[k];
        s.Reach[k] = bd.Reach[k];
        s.EyeToArm[k] = bd.EyeToUpperArmBody[k] > 0.0f ? bd.EyeToUpperArmBody[k] : 1e3f; // 0: not measured this frame
    }
    s.InsideRays = bd.ProbeRays >= 0 ? bd.ProbeInsideRays : 0;
    R.Fp.Push(s);
    // The gait cycle's bounce: the largest finished cycle since the action started.
    if (const float c = R.Fp.LastCycleMm(); c >= 0.0f && c != R.LastCycleSeen) {
        R.LastCycleSeen = c;
        if (R.Fp.Time() > R.FpFrom && CbResting(s.State)) R.CycleMm = std::max(R.CycleMm, c);
    }
    const WeaponJankFrame tf = WeaponJankFrameFor(world, body, p, dt, "");
    R.Tp.Push(tf);
    if (R.Fp.Time() > R.FpFrom && s.Valid) {
        R.SupGripFp = std::max(R.SupGripFp, s.HandGap[0]);
        R.SupReach = std::max(R.SupReach, s.Reach[0]);
        if (tf.Valid) R.SupGripTp = std::max(R.SupGripTp, tf.HandGap[0]);
    }
}

void NpcTest::CombosAfter(NpcDirector& npcs, const Player& player) {
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead) { up->Roam = false; up->WantMove = false; }
    // The Scene view: the world body from its front right, fixed to the cells' heading (a turn in place turns the body, not the camera).
    const glm::vec3 feet = player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    const float yaw = glm::radians(kCbYaw);
    const glm::vec3 front(std::cos(yaw), 0.0f, std::sin(yaw)), up(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(front, up));
    m_CamPos = feet + front * 1.45f + right * 0.85f + up * 1.4f;
    LookAt(feet + up * 1.15f);
}
