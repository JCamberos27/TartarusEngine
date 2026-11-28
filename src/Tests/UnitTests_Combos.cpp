// Unit tests: --npc-test combos' plain helpers (the cell filter's wildcards, the cell list, the on-screen test).
// Registered by RunUnitTests through RegisterCombosTests.
#include "UnitTestSupport.h"
#include "AI/NpcTest_Combos.h"

#include <algorithm>

namespace {

bool Has(const std::vector<std::string>& ids, const std::string& id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

void TestCombosMatch() {
    CHECK(Combos::Match("ak/reload_tac/walk", "ak/reload_tac/walk"));
    CHECK(Combos::Match("ak/reload_tac/walk", "ak/reload_*/walk"));
    CHECK(Combos::Match("ak/reload_tac/walk", "*"));
    CHECK(Combos::Match("ak/reload_tac/walk", "ak/*"));
    CHECK(Combos::Match("ak/reload_tac/walk", "*walk"));
    CHECK(Combos::Match("ak/reload_tac/walk", "ak/**/walk"));
    CHECK(Combos::Match("ak/aim_in/idle>crouch", "*/aim_in/*>*"));
    CHECK(!Combos::Match("ak/reload_tac/walk", "ak/reload_tac"));
    CHECK(!Combos::Match("ak/reload_tac/walk", "870/*"));
    CHECK(!Combos::Match("ak/reload_tac/walk>idle", "*/walk"));      // the whole id, not a prefix
    CHECK(Combos::Match("ak/reload_tac/walk>idle", "*/walk*"));
    CHECK(!Combos::Match("ak", ""));
    CHECK(Combos::Match("", "*"));
    // Backtracking: the first '*' gives back what the rest needs.
    CHECK(Combos::Match("ak/swap_870/jog", "*a*/jog"));
    CHECK(!Combos::Match("ak/swap_870/jog", "*a*/walk"));
}

void TestCombosCells() {
    const std::vector<std::string> ak = Combos::CellIds("", "");
    CHECK(ak.size() == 121); // 13 actions (fire_hold: full auto) x 8 states, 2 swaps; + 3 changes x (reload_tac, aim_in, the first swap)
    CHECK(Combos::CellIds("ak", "") == ak);
    CHECK(!ak.empty() && ak.front() == "ak/hold/idle"); // state by state, the hold row first
    CHECK(Has(ak, "ak/fire_hold/sprint") && Has(ak, "ak/swap_870/jog") && Has(ak, "ak/swap_rev/jog"));
    CHECK(Has(ak, "ak/swap_870/idle>crouch") && !Has(ak, "ak/swap_rev/idle>crouch")); // the change: the first swap only
    CHECK(Has(ak, "ak/reload_tac/walk>idle") && !Has(ak, "ak/reload_empty/walk>idle"));
    CHECK(std::none_of(ak.begin(), ak.end(), [](const std::string& id) { return id.rfind("ak/", 0) != 0; }));
    // Every hold row comes before the other rows of its state.
    for (const char* state : {"/idle", "/walk", "/jog", "/sprint", "/crouch_idle", "/crouch_walk", "/turn", "/jump"}) {
        size_t hold = ak.size(), first = ak.size();
        for (size_t i = 0; i < ak.size(); ++i) {
            const bool ofState = ak[i].size() > std::string(state).size() && ak[i].compare(ak[i].size() - std::string(state).size(), std::string::npos, state) == 0;
            if (!ofState) continue;
            if (ak[i].find("/hold/") != std::string::npos) hold = i;
            else first = std::min(first, i);
        }
        CHECK(hold < first);
    }
    const std::vector<std::string> rev = Combos::CellIds("rev", "");
    CHECK(rev.size() == 113); // no full auto: 8 fewer
    CHECK(!Has(rev, "rev/fire_hold/idle") && Has(rev, "rev/swap_ak/idle") && Has(rev, "rev/swap_870/idle"));
    CHECK(Combos::CellIds("ak, rev", "").size() == 121 + 113);
    CHECK(Combos::CellIds("ak,nope", "").size() == 121); // an unknown weapon is skipped
    const std::vector<std::string> some = Combos::CellIds("ak,870", "*/reload_tac/walk, ak/melee/*");
    CHECK(some.size() == 2 + 8);
    CHECK(Has(some, "ak/reload_tac/walk") && Has(some, "870/reload_tac/walk") && Has(some, "ak/melee/jump"));
    CHECK(Combos::CellIds("ak", "nothing/like/this").empty());
}

void TestCombosOnScreen() {
    const float fov = 60.0f, aspect = 16.0f / 9.0f; // tan(30 deg) = 0.577
    CHECK(Combos::OnScreen(glm::vec3(0.0f, 0.0f, -0.5f), fov, aspect, 0.0f));
    CHECK(!Combos::OnScreen(glm::vec3(0.0f, 0.0f, 0.5f), fov, aspect, 0.0f));   // behind the eye
    CHECK(!Combos::OnScreen(glm::vec3(0.0f, 0.0f, -0.01f), fov, aspect, 0.0f)); // at the eye
    // Below the view: at 0.4 m deep the bottom edge is 0.231 m down.
    CHECK(Combos::OnScreen(glm::vec3(0.0f, -0.22f, -0.4f), fov, aspect, 0.0f));
    CHECK(!Combos::OnScreen(glm::vec3(0.0f, -0.25f, -0.4f), fov, aspect, 0.0f));
    CHECK(Combos::OnScreen(glm::vec3(0.0f, -0.25f, -0.4f), fov, aspect, 0.1f)); // within the 10% slack (0.254)
    // Sideways the width is the height x the aspect (0.410 m at 0.4 m deep).
    CHECK(Combos::OnScreen(glm::vec3(0.40f, 0.0f, -0.4f), fov, aspect, 0.0f));
    CHECK(!Combos::OnScreen(glm::vec3(-0.42f, 0.0f, -0.4f), fov, aspect, 0.0f));
    CHECK(!Combos::OnScreen(glm::vec3(0.40f, 0.0f, -0.4f), fov, 1.0f, 0.0f));
}

} // namespace

void RegisterCombosTests(UnitTestSupport::TestList& tests) {
    tests.insert(tests.end(), {
        {"CombosMatch", TestCombosMatch},
        {"CombosCells", TestCombosCells},
        {"CombosOnScreen", TestCombosOnScreen},
    });
}
