#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "GIData.h" // GI::Box

// GI Phase 1 (W2): which local lights get a shadow slot, and how a slot fades in and out. Kept free of the registry and
// GL so the unit test can drive it directly (UnitTests_Render.cpp, TestShadowSlotScoring).
//
// Rooms come from the scene's GI Volumes: one box per volume, grouped into a room by name with any trailing number
// dropped ("GI living room 1" and "GI living room 2" are one room). The room the view is in, and the rooms whose
// boxes touch it, light the player; their lamps score ahead of lamps elsewhere in the house, so a full budget goes
// to the room you are standing in before the one behind the wall. Outside every volume, no lamp is boosted and the
// score is the old #110 distance score.
namespace ShadowSlotScoring {

constexpr float kRoomBoost = 16.0f;        // a lamp in the view's room
constexpr float kNeighbourBoost = 4.0f;    // a lamp in a room whose box touches it
constexpr float kNeighbourGap = 0.35f;     // metres between two rooms' boxes that still count as touching (a wall)
constexpr float kLightPad = 0.2f;          // a light this far outside a room's box still counts as in it
constexpr float kStickyPad = 1.5f;         // the view keeps its last room this far out of it (a doorway gap)
constexpr float kMinStoreyOverlap = 0.5f;  // two rooms only neighbour if their heights overlap this much (not through a floor)
constexpr float kHoldBonus = 1.25f;        // last frame's holders (hysteresis, #110)
constexpr float kFadeSeconds = 0.25f;      // a shadow fades in, and out, over this long instead of popping

enum class Tier : std::uint8_t { Other = 0, Neighbour = 1, Room = 2 };

struct RoomBox {
    GI::Box Box;
    int Room = -1; // index into Rooms::Names
};

struct Rooms {
    std::vector<RoomBox> Boxes;
    std::vector<std::string> Names;  // one per room
    int ViewRoom = -1;               // the room holding the view, or -1 (outside every volume)
    std::vector<Tier> RoomTier;      // per room, relative to ViewRoom
    // The last room the view was in, kept across Clear(): a doorway is often in no volume (P0: the GI gaps), and
    // dropping to "no room" there re-ranked every lamp twice per door crossing. The view keeps it until it enters
    // another room or goes kStickyPad past it.
    std::string StickyRoom;

    void Clear() { Boxes.clear(); Names.clear(); RoomTier.clear(); ViewRoom = -1; }
    // Forget the sticky room as well (a scene load, a camera cut).
    void Reset() { Clear(); StickyRoom.clear(); }
    // Adds a volume's box under its room (by RoomKey of its name).
    void Add(const std::string& volumeName, const GI::Box& box);
    // Finds the view's room (the smallest box holding `viewPos`, else the sticky room if the view is still near it)
    // and ranks every room against it.
    void Classify(const glm::vec3& viewPos);
    // The best tier of any box holding `p` (Other outside them all).
    Tier TierAt(const glm::vec3& p) const;
};

// "GI living room 2" -> "GI living room": the name less a trailing run of digits and the spaces/underscores before it.
std::string RoomKey(const std::string& volumeName);

// True when `p` is inside `b`, grown by `pad` metres on every face.
bool Inside(const GI::Box& b, const glm::vec3& p, float pad = 0.0f);
// True when the two boxes are within `gap` metres of each other (separating-axis test on the grown boxes).
bool Touch(const GI::Box& a, const GI::Box& b, float gap);
// How far the two boxes' world height ranges overlap (negative: the gap between them).
float StoreyOverlap(const GI::Box& a, const GI::Box& b);

float TierBoost(Tier t);

// The light's score: brightness reaching the view (intensity * range^2 over the squared gap from the view to the
// light's sphere, #110), times its room's boost, times kHoldBonus for last frame's holders.
float Score(float intensity, float range, float viewToLight, Tier tier, bool heldLastFrame);

// Per-slot fades. A slot's light is either wanted (fading in to 1) or leaving (fading out to 0, and then the slot is
// freed). A light that was lit but slot-less only gets a slot that is free, so it waits while a leaving light fades
// out and then fades in: the hand-over between two lit lamps never pops.
// A light that ARRIVES - not LIT last frame: switched on, activated, spawned - is different: its light appears at
// once, so its shadow must too, or it shines through walls until the fade is done (the #1189 review). It comes in
// at full strength, taking the leaving holder with the least shadow left if no slot is free. The flashlight (urgent)
// is always treated so. "Lit" is taken before the distance cull: a lamp walking into DistanceFade range was lit all
// along and its light ramps up from 0, so its shadow fades in like any hand-over (the review's re-check).
template <int N>
struct FadingSlots {
    std::uint32_t Owner[N];        // the light's key, or kNone
    float Weight[N] = {};          // 0..1, how much of the shadow shows
    std::uint64_t Used[N] = {};    // last frame the slot was held (a new light takes the free slot unused longest)
    std::vector<std::uint32_t> PrevLit; // last Step's lit lights (sorted)
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    FadingSlots() { Reset(); }
    // Forget every holder (a scene load: entity ids are about to mean other lights).
    void Reset() {
        for (auto& o : Owner) o = kNone;
        for (auto& w : Weight) w = 0.0f;
        for (auto& u : Used) u = 0;
        PrevLit.clear();
    }

    // Every candidate is lit and nothing else is (no distance cull): the unit tests' short form.
    int Step(const std::vector<std::uint32_t>& want, const std::vector<std::uint32_t>& candidates, int budget,
             std::uint64_t frame, float dt, bool snap, std::uint32_t urgent = kNone) {
        return StepLit(want, candidates, candidates, budget, frame, dt, snap, urgent);
    }
    // want: the lights that should hold a shadow, best first (at most `budget` of them).
    // candidates: every light that could hold one this frame (sorted); a holder that is not a candidate any more
    //   (switched off, culled, gone) is freed at once, since there is no light left to fade.
    // lit: every shadowed light that is on this frame, before the distance cull (sorted; holds the candidates).
    // snap: no fades this frame (the GI bake, the first frame, a camera cut, a scene load).
    // urgent: a key that is always treated as arriving (the flashlight).
    // Writes slot -> owner for every slot in use (Owner/Weight), and returns how many slots changed hands.
    int StepLit(const std::vector<std::uint32_t>& want, const std::vector<std::uint32_t>& candidates,
             const std::vector<std::uint32_t>& lit, int budget, std::uint64_t frame, float dt, bool snap,
             std::uint32_t urgent = kNone) {
        budget = budget < 0 ? 0 : (budget > N ? N : budget);
        int changes = 0;
        for (int i = budget; i < N; ++i)
            if (Owner[i] != kNone) { Owner[i] = kNone; Weight[i] = 0.0f; ++changes; }
        const float step = snap || kFadeSeconds <= 0.0f ? 1.0f : dt / kFadeSeconds;
        auto wanted = [&](std::uint32_t k) {
            for (std::uint32_t w : want) if (w == k) return true;
            return false;
        };
        auto in = [](const std::vector<std::uint32_t>& sorted, std::uint32_t k) {
            std::size_t lo = 0, hi = sorted.size();
            while (lo < hi) { const std::size_t mid = (lo + hi) / 2; if (sorted[mid] < k) lo = mid + 1; else hi = mid; }
            return lo < sorted.size() && sorted[lo] == k;
        };
        auto candidate = [&](std::uint32_t k) { return in(candidates, k); };
        auto arriving = [&](std::uint32_t k) { return k == urgent || !in(PrevLit, k); };
        // Holders: fade in if wanted, fade out (then free) if not.
        for (int i = 0; i < budget; ++i) {
            if (Owner[i] == kNone) continue;
            if (wanted(Owner[i])) {
                Weight[i] = Weight[i] + step > 1.0f ? 1.0f : Weight[i] + step;
                Used[i] = frame;
            } else if (snap || !candidate(Owner[i])) {
                Owner[i] = kNone; Weight[i] = 0.0f; ++changes;
            } else {
                Weight[i] -= step;
                Used[i] = frame;
                if (Weight[i] <= 0.0f) { Owner[i] = kNone; Weight[i] = 0.0f; ++changes; }
            }
        }
        // Newcomers take the free slot unused longest, arriving lights first (they can't wait), then in `want` order.
        // An arriving light comes in at full strength; a lit one fades in (both full on a snap).
        for (int pass = 0; pass < 2; ++pass) {
            for (std::uint32_t k : want) {
                const bool now = arriving(k);
                if (now != (pass == 0)) continue;
                bool held = false;
                for (int i = 0; i < budget && !held; ++i) held = Owner[i] == k;
                if (held) continue;
                int pick = -1;
                for (int i = 0; i < budget; ++i)
                    if (Owner[i] == kNone && (pick < 0 || Used[i] < Used[pick])) pick = i;
                if (pick < 0 && now) // full: an arriving light takes the leaving slot with the least shadow left
                    for (int i = 0; i < budget; ++i)
                        if (!wanted(Owner[i]) && (pick < 0 || Weight[i] < Weight[pick])) pick = i;
                if (pick < 0) continue; // full: a lit light waits for a leaving light to fade out
                Owner[pick] = k; Used[pick] = frame; ++changes;
                Weight[pick] = snap || now ? 1.0f : step;
            }
        }
        if (urgent != kNone)
            for (int i = 0; i < budget; ++i)
                if (Owner[i] == urgent) Weight[i] = 1.0f;
        PrevLit = lit;
        return changes;
    }
};

} // namespace ShadowSlotScoring
