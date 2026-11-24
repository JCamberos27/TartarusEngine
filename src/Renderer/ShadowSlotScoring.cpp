#include "ShadowSlotScoring.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ShadowSlotScoring {

std::string RoomKey(const std::string& volumeName) {
    std::size_t end = volumeName.size();
    while (end > 0 && std::isdigit((unsigned char)volumeName[end - 1])) --end;
    if (end == volumeName.size()) return volumeName; // no trailing number
    while (end > 0 && (volumeName[end - 1] == ' ' || volumeName[end - 1] == '_')) --end;
    return end > 0 ? volumeName.substr(0, end) : volumeName; // all digits: keep it as is
}

bool Inside(const GI::Box& b, const glm::vec3& p, float pad) {
    const glm::vec3 l = b.ToLocal(p);
    return std::abs(l.x) <= b.Half.x + pad && std::abs(l.y) <= b.Half.y + pad && std::abs(l.z) <= b.Half.z + pad;
}

bool Touch(const GI::Box& a, const GI::Box& b, float gap) {
    // Separating-axis test on two oriented boxes, each grown by gap/2: the 3 + 3 face axes and the 9 edge crosses.
    const glm::vec3 ha = a.Half + glm::vec3(0.5f * gap), hb = b.Half + glm::vec3(0.5f * gap);
    const glm::vec3 d = b.Center - a.Center;
    auto separated = [&](glm::vec3 axis) {
        const float len2 = glm::dot(axis, axis);
        if (len2 < 1e-10f) return false; // parallel edges: the face axes cover it
        axis /= std::sqrt(len2);
        float ra = 0.0f, rb = 0.0f;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] * std::abs(glm::dot(a.Axis[i], axis));
            rb += hb[i] * std::abs(glm::dot(b.Axis[i], axis));
        }
        return std::abs(glm::dot(d, axis)) > ra + rb;
    };
    for (int i = 0; i < 3; ++i)
        if (separated(a.Axis[i]) || separated(b.Axis[i])) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (separated(glm::cross(a.Axis[i], b.Axis[j]))) return false;
    return true;
}

void Rooms::Add(const std::string& volumeName, const GI::Box& box) {
    const std::string key = RoomKey(volumeName);
    int room = -1;
    for (int i = 0; i < (int)Names.size(); ++i)
        if (Names[i] == key) { room = i; break; }
    if (room < 0) { room = (int)Names.size(); Names.push_back(key); }
    Boxes.push_back({box, room});
}

float StoreyOverlap(const GI::Box& a, const GI::Box& b) {
    auto yHalf = [](const GI::Box& x) {
        return std::abs(x.Axis[0].y) * x.Half.x + std::abs(x.Axis[1].y) * x.Half.y + std::abs(x.Axis[2].y) * x.Half.z;
    };
    const float ha = yHalf(a), hb = yHalf(b);
    return std::min(a.Center.y + ha, b.Center.y + hb) - std::max(a.Center.y - ha, b.Center.y - hb);
}

void Rooms::Classify(const glm::vec3& viewPos) {
    ViewRoom = -1;
    RoomTier.assign(Names.size(), Tier::Other);
    float best = 0.0f;
    for (const auto& rb : Boxes) {
        if (!Inside(rb.Box, viewPos)) continue;
        const float vol = rb.Box.Half.x * rb.Box.Half.y * rb.Box.Half.z;
        if (ViewRoom < 0 || vol < best) { ViewRoom = rb.Room; best = vol; }
    }
    if (ViewRoom < 0 && !StickyRoom.empty()) // in no volume (a doorway gap): keep the last room while still near it
        for (const auto& rb : Boxes)
            if (Names[rb.Room] == StickyRoom && Inside(rb.Box, viewPos, kStickyPad)) { ViewRoom = rb.Room; break; }
    if (ViewRoom < 0) { StickyRoom.clear(); return; }
    StickyRoom = Names[ViewRoom];
    RoomTier[ViewRoom] = Tier::Room;
    for (const auto& mine : Boxes) {
        if (mine.Room != ViewRoom) continue;
        for (const auto& other : Boxes)
            if (other.Room != ViewRoom && RoomTier[other.Room] == Tier::Other && Touch(mine.Box, other.Box, kNeighbourGap) &&
                StoreyOverlap(mine.Box, other.Box) >= kMinStoreyOverlap) // not the room through the floor slab
                RoomTier[other.Room] = Tier::Neighbour;
    }
}

Tier Rooms::TierAt(const glm::vec3& p) const {
    Tier t = Tier::Other;
    if (ViewRoom < 0) return t;
    // A light inside a box belongs to that box's room(s). Only a light inside none is matched with the boxes padded by
    // kLightPad: a ceiling fitting sits at, or just past, the top face (the boxes stop a few cm into it). Padding
    // first would hand a wall lamp just through the wall to the room on the other side.
    bool inAny = false;
    for (const auto& rb : Boxes)
        if (Inside(rb.Box, p)) { inAny = true; if (RoomTier[rb.Room] > t) t = RoomTier[rb.Room]; }
    if (inAny) return t;
    for (const auto& rb : Boxes)
        if (RoomTier[rb.Room] > t && Inside(rb.Box, p, kLightPad)) t = RoomTier[rb.Room];
    return t;
}

float TierBoost(Tier t) {
    switch (t) {
    case Tier::Room: return kRoomBoost;
    case Tier::Neighbour: return kNeighbourBoost;
    default: return 1.0f;
    }
}

float Score(float intensity, float range, float viewToLight, Tier tier, bool heldLastFrame) {
    range = std::max(range, 0.2f);
    const float gap = std::max(viewToLight - range, 0.0f);
    float s = intensity * range * range / (gap * gap + 1.0f) * TierBoost(tier);
    if (heldLastFrame) s *= kHoldBonus;
    return s;
}

} // namespace ShadowSlotScoring
