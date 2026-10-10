#include "WeaponLab.h"

#include "Camera.h"
#include "Components.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "Model.h"
#include "World.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

WeaponJankFrame WeaponJankFrameFor(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p, float dt,
                                   const std::string& label) {
    WeaponJankFrame f;
    f.Dt = dt;
    const std::string& state = p.CurrentState();
    f.State = label.empty() ? state : label + " / " + state;
    f.Resting = state == "Idle" || state == "Aim" || state == "Walk" || state == "Sprint";
    f.Recoil = p.SinceShot() < 0.35f;
    f.SinceShot = p.SinceShot();
    f.Speed = p.PlanarSpeed();
    f.View = glm::normalize(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(p.ArmsWorld()[0])), glm::normalize(glm::vec3(p.ArmsWorld()[1])),
                                                     glm::normalize(glm::vec3(p.ArmsWorld()[2])))));
    auto rigid = [](const glm::mat4& m) {
        glm::mat4 r(1.0f);
        for (int k = 0; k < 3; ++k) r[k] = glm::vec4(glm::normalize(glm::vec3(m[k])), 0.0f);
        r[3] = m[3];
        return r;
    };
    const Model* clips = p.ThirdPersonArms();
    const entt::entity gun = p.WorldWeaponEntity();
    glm::mat4 socket(1.0f);
    f.Valid = clips && gun != entt::null && world.Registry.valid(gun) && clips->NodeTransform(p.GunSocket(), socket) &&
              !world.Registry.any_of<InactiveTag, DeactivatedTag>(gun);
    if (!f.Valid) return f;
    const glm::mat4 gunWorld = rigid(world.ComposeWorldTransform(gun));
    const glm::mat4 clipGun = rigid(p.ArmsWorld() * socket);
    const glm::mat4 r = glm::inverse(clipGun) * gunWorld;
    f.GunPos = glm::vec3(r[3]);
    f.GunRot = glm::normalize(glm::quat_cast(glm::mat3(r)));
    const WeaponIKSettings& ik = p.Set().Procedural.IK;
    const std::string clipHand[2] = {ik.LeftHand, ik.RightHand};
    glm::vec3 chest(0.0f);
    body.BoneWorld(world, "spine_05", chest);
    for (int h = 0; h < 2; ++h) {
        glm::vec3 w;
        glm::mat4 ch(1.0f);
        if (body.BoneWorld(world, h ? "hand_r" : "hand_l", w) && clips->NodeTransform(clipHand[h], ch)) {
            f.Hand[h] = w - chest;
            f.HandClip[h] = glm::vec3(glm::inverse(clipGun) * p.ArmsWorld() * ch[3]);
        }
        f.HandGap[h] = body.TwinHandGap(h);
    }
    return f;
}

namespace {
const char* kViewNames[] = {"Front right", "Right", "Front left", "Back right", "Left arm", "Right arm", "Swinging", "Free"};
} // namespace

void WeaponLab::AfterPose(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p, const Camera& player, float dt) {
    m_HaveCam = false;
    if (!p.IsActive()) return;
    // The hold's readouts and the jank meter.
    m_State = p.CurrentState();
    FirstPersonWorldGunInput gi;
    m_Lock = p.WorldGunInput(gi) ? gi.Shouldered : 0.0f;
    m_Anchor = body.TwinAnchorWeight();
    for (int h = 0; h < 2; ++h) m_HandGap[h] = body.TwinHandGap(h);
    m_Spare = p.SpareMagazineShown() ? p.SpareMagazineGap() : -1.0f;
    if (dt > 0.0f) {
        const WeaponJankFrame f = WeaponJankFrameFor(world, body, p, dt, "");
        m_Jank.Push(f);
        m_GunJump = f.Valid && m_HaveLastGun ? glm::length(f.GunPos - m_LastGun) * 100.0f : 0.0f;
        m_HaveLastGun = f.Valid;
        m_LastGun = f.GunPos;
        m_Trace[m_TraceAt] = m_GunJump;
        m_TraceAt = (m_TraceAt + 1) % (int)(sizeof m_Trace / sizeof m_Trace[0]);
    }
    if (m_View == View::Free) return;
    // The Scene camera: on the world body, from the body's facing (the player's view, flat).
    glm::vec3 neck;
    if (!body.BoneWorld(world, "neck_01", neck)) return;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    glm::vec3 front = player.Front();
    front.y = 0.0f;
    if (glm::length(front) < 1e-4f) return;
    front = glm::normalize(front);
    const glm::vec3 right = glm::normalize(glm::cross(front, up));
    // Steady: the target's height off the eye (the view is steadied), not the neck's bob.
    glm::vec3 target(neck.x, player.Position.y - 0.4f, neck.z);
    glm::vec3 dir = glm::normalize(front + right * 0.75f);
    float dist = 2.3f, lift = 0.15f;
    switch (m_View) {
    case View::Right: dir = right; break;
    case View::FrontLeft: dir = glm::normalize(front - right * 0.85f); break;
    case View::BackRight: dir = glm::normalize(-front + right * 0.75f); break;
    case View::LeftArm:
    case View::RightArm: {
        const bool l = m_View == View::LeftArm;
        glm::vec3 a, b;
        if (body.BoneWorld(world, l ? "upperarm_l" : "upperarm_r", a) && body.BoneWorld(world, l ? "hand_l" : "hand_r", b)) {
            target = 0.5f * (a + b);
            dir = glm::normalize(front * 0.8f + (l ? -right : right) * 0.6f + up * 0.15f);
            dist = 0.9f;
            lift = 0.0f;
        }
        break;
    }
    case View::Orbit: { // swinging round the front, from the left side to behind the right shoulder and back every 10 s
        m_OrbitDeg = std::fmod(m_OrbitDeg + 36.0f * dt, 360.0f);
        const float a = glm::radians(25.0f + 85.0f * std::sin(glm::radians(m_OrbitDeg)));
        dir = glm::normalize(front * std::cos(a) + right * std::sin(a));
        target.y = neck.y - 0.3f;
        dist = 1.5f;
        lift = 0.25f;
        break;
    }
    default: break;
    }
    m_CamPos = target + dir * dist + up * lift;
    m_CamTarget = target;
    m_HaveCam = true;
}

bool WeaponLab::SceneCamera(glm::vec3& position, float& yaw, float& pitch) const {
    if (!m_HaveCam) return false;
    const glm::vec3 look = glm::normalize(m_CamTarget - m_CamPos);
    position = m_CamPos;
    yaw = glm::degrees(std::atan2(look.z, look.x));
    pitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
    return true;
}

void WeaponLab::HandleKeys(GLFWwindow* window) {
    if (!window) return;
    auto pressed = [&](int slot, int key) {
        const bool down = glfwGetKey(window, key) == GLFW_PRESS;
        const bool edge = down && !m_KeyWas[slot];
        m_KeyWas[slot] = down;
        return edge;
    };
    const View views[] = {View::Free, View::FrontRight, View::Right, View::FrontLeft, View::BackRight, View::LeftArm, View::RightArm, View::Orbit};
    for (int k = 0; k <= 7; ++k)
        if (pressed(k, GLFW_KEY_KP_0 + k)) m_View = views[k];
    static const float kScales[] = {0.1f, 0.25f, 0.5f, 1.0f};
    int at = 3;
    for (int k = 0; k < 4; ++k) if (std::abs(m_TimeScale - kScales[k]) < 1e-3f) at = k;
    if (pressed(8, GLFW_KEY_KP_ADD)) m_TimeScale = kScales[std::min(at + 1, 3)];
    if (pressed(9, GLFW_KEY_KP_SUBTRACT)) m_TimeScale = kScales[std::max(at - 1, 0)];
    if (pressed(10, GLFW_KEY_KP_ENTER)) m_PauseToggle = true;
    if (pressed(11, GLFW_KEY_KP_DECIMAL)) m_Step = true;
}

void WeaponLab::OnPlayStopped() {
    m_Jank.Reset();
    m_HaveCam = m_HaveLastGun = false;
    m_TimeScale = 1.0f;
}

void WeaponLab::DrawPanel(FirstPersonPresentation* p, bool playing) {
    ImGui::SetNextWindowSize(ImVec2(430.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Weapon Lab")) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Scene view: the world body. Game view: your own. Keypad 0-7 views, +/- speed, Enter pause, . step.");
    ImGui::SeparatorText("Scene camera");
    for (int k = 0; k < (int)View::Count; ++k) {
        if (k % 4) ImGui::SameLine();
        const bool on = (int)m_View == k;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kViewNames[k], ImVec2(98.0f, 0.0f))) m_View = (View)k;
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SeparatorText("Time");
    for (float s : {1.0f, 0.5f, 0.25f, 0.1f}) {
        char label[16];
        std::snprintf(label, sizeof label, "%gx", s);
        if (s != 1.0f) ImGui::SameLine();
        const bool on = std::abs(m_TimeScale - s) < 1e-3f;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label, ImVec2(56.0f, 0.0f))) m_TimeScale = s;
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) m_PauseToggle = true;
    ImGui::SameLine();
    if (ImGui::Button("Step")) m_Step = true;

    ImGui::SeparatorText("Actions");
    if (!playing || !p || !p->IsActive()) {
        ImGui::TextDisabled("Press Play.");
    } else {
        auto act = [&](const char* name, auto&& go) {
            if (ImGui::Button(name, ImVec2(98.0f, 0.0f))) {
                go();
                m_LastAction = name;
            }
        };
        act("Reload", [&] { p->SetAmmo(std::max(1, p->MagazineSize() / 2)); p->Reload(); });
        ImGui::SameLine();
        act("Empty reload", [&] { p->SetAmmo(0); p->Reload(); });
        ImGui::SameLine();
        act("Mag check", [&] { p->TriggerAction("MagCheck"); });
        ImGui::SameLine();
        act("Inspect", [&] { p->TriggerAction("Inspect"); });
        act("Fidget", [&] { p->TriggerAction("Fidget"); });
        ImGui::SameLine();
        act("Melee", [&] { p->TriggerAction("Melee"); });
        ImGui::SameLine();
        act("Switch gun", [&] { if (p->SlotCount() > 1) p->SelectSlot((p->Slot() + 1) % p->SlotCount()); });
        ImGui::SameLine();
        act("Refill", [&] { p->RefillAmmo(); });
        ImGui::TextDisabled("Last: %s", m_LastAction.c_str());

        ImGui::SeparatorText("Third-person hold");
        ImGui::Text("State      %s", m_State.c_str());
        ImGui::Text("Stock lock %.2f    Hand anchor %.2f", m_Lock, m_Anchor);
        ImGui::Text("Hands off grip  L %.1f cm  R %.1f cm", m_HandGap[0] * 100.0f, m_HandGap[1] * 100.0f);
        if (m_Spare >= 0.0f) ImGui::Text("Spare mag / shell in hand: %.1f cm from it", m_Spare * 100.0f);
        else ImGui::TextDisabled("Spare mag / shell: hidden");
        const int n = (int)(sizeof m_Trace / sizeof m_Trace[0]);
        ImGui::PlotLines("##gun", m_Trace, n, m_TraceAt, "world gun's own motion (cm/frame)", 0.0f, 2.0f, ImVec2(-1.0f, 60.0f));
    }

    ImGui::SeparatorText("Jank");
    const auto& ev = m_Jank.Events();
    ImGui::Text("%zu flagged", ev.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) m_Jank.Reset();
    ImGui::BeginChild("##jank", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    for (int i = (int)ev.size() - 1, shown = 0; i >= 0 && shown < 40; --i, ++shown) ImGui::TextUnformatted(ev[i].Describe().c_str());
    ImGui::EndChild();
    ImGui::End();
}
