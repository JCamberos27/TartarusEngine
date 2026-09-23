#include "FirstPersonProcedural.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

namespace {

float Pick(std::mt19937& rng, const glm::vec2& range) {
    if (range.x == range.y) return range.x;
    return std::uniform_real_distribution<float>(std::min(range.x, range.y), std::max(range.x, range.y))(rng);
}

float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float MoveToward(float from, float to, float step) {
    if (from < to) return std::min(to, from + step);
    return std::max(to, from - step);
}

glm::vec3 ClampLength(const glm::vec3& v, float maxLen) {
    const float len = glm::length(v);
    return len > maxLen && len > 1e-9f ? v * (maxLen / len) : v;
}

// --- JSON ------------------------------------------------------------------------------------

json Vec(const glm::vec2& v) { return json::array({v.x, v.y}); }
json Vec(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

struct Reader {
    const json& J;
    std::string* Error;
    bool Ok = true;

    bool Fail(const std::string& message) {
        if (Ok && Error) *Error = message;
        Ok = false;
        return false;
    }
    void Number(const char* key, float& out) {
        const auto it = J.find(key);
        if (it == J.end()) return;
        if (!it->is_number() || !std::isfinite(it->get<float>())) { Fail(std::string("'") + key + "' must be a number"); return; }
        out = it->get<float>();
    }
    void Bool(const char* key, bool& out) {
        const auto it = J.find(key);
        if (it != J.end() && it->is_boolean()) out = it->get<bool>();
    }
    void String(const char* key, std::string& out) {
        const auto it = J.find(key);
        if (it != J.end() && it->is_string()) out = it->get<std::string>();
    }
    template <int N>
    void Vector(const char* key, glm::vec<N, float>& out) {
        const auto it = J.find(key);
        if (it == J.end()) return;
        if (!it->is_array() || it->size() != (size_t)N) { Fail(std::string("'") + key + "' must be " + std::to_string(N) + " numbers"); return; }
        glm::vec<N, float> v;
        for (int i = 0; i < N; ++i) {
            if (!(*it)[i].is_number() || !std::isfinite((*it)[i].get<float>())) { Fail(std::string("'") + key + "' must be finite numbers"); return; }
            v[i] = (*it)[i].get<float>();
        }
        out = v;
    }
    void CurveField(const char* key, Curve& out) {
        const auto it = J.find(key);
        if (it == J.end()) return;
        if (!Curve::FromJson(*it, out)) Fail(std::string("'") + key + "' must be a curve: [[time, value, inTangent, outTangent], ...]");
    }
    void Curve3(const char* key, FirstPersonCurve3& out) {
        const auto it = J.find(key);
        if (it == J.end()) return;
        if (!it->is_object()) { Fail(std::string("'") + key + "' must be an object of x/y/z curves"); return; }
        Reader r{*it, Error};
        r.CurveField("x", out.X);
        r.CurveField("y", out.Y);
        r.CurveField("z", out.Z);
        if (!r.Ok) Ok = false;
    }
    void Spring(const char* key, FirstPersonSpring& out) {
        const auto it = J.find(key);
        if (it == J.end() || !it->is_object()) return;
        Reader r{*it, Error};
        r.Number("frequency", out.Frequency);
        r.Number("damping", out.Damping);
        if (!r.Ok) Ok = false;
    }
    // Runs `fn` on the sub-object `key` when present.
    template <class Fn>
    void Object(const char* key, Fn&& fn) {
        const auto it = J.find(key);
        if (it == J.end()) return;
        if (!it->is_object()) { Fail(std::string("'") + key + "' must be an object"); return; }
        Reader r{*it, Error};
        fn(r);
        if (!r.Ok) Ok = false;
    }
};

json Curve3Json(const FirstPersonCurve3& c) { return {{"x", c.X.ToJson()}, {"y", c.Y.ToJson()}, {"z", c.Z.ToJson()}}; }
json SpringJson(const FirstPersonSpring& s) { return {{"frequency", s.Frequency}, {"damping", s.Damping}}; }

} // namespace

// ---------------------------------------------------------------------------------------------

// A recoil shot: a straight rise to 1 at `peak`, then an exponential settle (time constant `tau`)
// that is ~0 by 1. Keys carry the exact slopes, so the Hermite curve follows the function.
Curve FirstPersonKickCurve(float amplitude, float peak, float tau) {
    peak = std::clamp(peak, 0.005f, 0.9f);
    tau = std::max(tau, 0.005f);
    Curve c;
    c.Keys.push_back({0.0f, 0.0f, 1.0f / peak, 1.0f / peak});
    c.Keys.push_back({peak, 1.0f, 1.0f / peak, -1.0f / tau});
    for (float k : {0.5f, 1.0f, 2.0f, 3.0f}) {
        const float t = peak + tau * k;
        if (t >= 1.0f) break;
        const float v = std::exp(-k);
        c.Keys.push_back({t, v, -v / tau, -v / tau});
    }
    c.Keys.push_back({1.0f, 0.0f, 0.0f, 0.0f});
    for (auto& key : c.Keys) { key.Value *= amplitude; key.InTangent *= amplitude; key.OutTangent *= amplitude; }
    return c;
}

// amplitude * sin(2 pi (cycles * p) + phase) over p in 0..1, exact slopes at 8 keys per cycle.
Curve FirstPersonSineCurve(float amplitude, float cycles, float phase) {
    Curve c;
    const int n = std::max(2, (int)std::ceil(8.0f * cycles));
    const float w = glm::two_pi<float>() * cycles;
    for (int i = 0; i <= n; ++i) {
        const float p = (float)i / (float)n;
        const float slope = amplitude * w * std::cos(w * p + phase);
        c.Keys.push_back({p, amplitude * std::sin(w * p + phase), slope, slope});
    }
    return c;
}

WeaponProceduralSettings WeaponProceduralSettings::Defaults() {
    WeaponProceduralSettings s;
    auto& r = s.Recoil;
    r.Rotation.X = FirstPersonKickCurve(1.2f);   // pitch: the old ADS kick's 1.2 degrees
    r.Rotation.Y = FirstPersonKickCurve(0.25f);
    r.Rotation.Z = FirstPersonKickCurve(1.5f);
    r.Position.X = FirstPersonKickCurve(0.0012f);
    r.Position.Y = FirstPersonKickCurve(0.002f);
    r.Position.Z = FirstPersonKickCurve(0.014f); // kickback into the shoulder
    r.CameraPitch = FirstPersonKickCurve(0.45f, 0.08f, 0.16f);
    r.CameraYaw = FirstPersonKickCurve(0.15f, 0.08f, 0.16f);

    auto& b = s.Bob;
    // A figure-eight: one side swing and two dips per stride (the old ADS bob's numbers).
    b.Walk.X = FirstPersonSineCurve(0.003f);
    b.Walk.Y = FirstPersonSineCurve(0.0015f, 2.0f);
    b.Walk.Z = FirstPersonSineCurve(0.35f);
    b.Sprint.X = FirstPersonSineCurve(0.008f);
    b.Sprint.Y = FirstPersonSineCurve(0.005f, 2.0f);
    b.Sprint.Z = FirstPersonSineCurve(1.2f);
    b.HipScale = 0.5f;               // the walk/sprint clips already bob at the hip

    auto& br = s.Breath;
    br.Position.X = FirstPersonSineCurve(0.0003f, 1.0f, glm::half_pi<float>());
    br.Position.Y = FirstPersonSineCurve(0.0006f);
    br.Pitch = FirstPersonSineCurve(0.12f);

    s.Aim.Blend = Curve::EaseInOut();
    s.StateOffsets = {{"Walk", {}, {}, 0.2f, 0.25f}, {"Sprint", {}, {}, 0.2f, 0.25f}};
    return s;
}

json WeaponProceduralSettings::ToJson() const {
    json j;
    const auto& r = Recoil;
    j["recoil"] = {
        {"enabled", r.Enabled}, {"duration", r.Duration},
        {"rotation", Curve3Json(r.Rotation)}, {"position", Curve3Json(r.Position)},
        {"pitchRange", Vec(r.PitchRange)}, {"yawRange", Vec(r.YawRange)}, {"rollRange", Vec(r.RollRange)},
        {"sideRange", Vec(r.SideRange)}, {"upRange", Vec(r.UpRange)}, {"kickRange", Vec(r.KickRange)},
        {"hipScale", r.HipScale}, {"adsScale", r.AdsScale}, {"pivot", Vec(r.Pivot)},
        {"smoothing", SpringJson(r.Smoothing)},
        {"cameraPitch", r.CameraPitch.ToJson()}, {"cameraYaw", r.CameraYaw.ToJson()},
        {"cameraYawRange", Vec(r.CameraYawRange)}, {"cameraScale", r.CameraScale},
    };
    const auto& w = Sway;
    j["sway"] = {
        {"enabled", w.Enabled}, {"lookRotation", w.LookRotation}, {"lookPosition", w.LookPosition},
        {"maxRotation", w.MaxRotation}, {"maxPosition", w.MaxPosition},
        {"movePosition", w.MovePosition}, {"moveRoll", w.MoveRoll}, {"adsScale", w.AdsScale},
        {"spring", SpringJson(w.Spring)},
    };
    const auto& b = Bob;
    j["bob"] = {
        {"enabled", b.Enabled}, {"walkStride", b.WalkStride}, {"sprintStride", b.SprintStride},
        {"walk", Curve3Json(b.Walk)}, {"sprint", Curve3Json(b.Sprint)}, {"walkFullSpeed", b.WalkFullSpeed},
        {"hipScale", b.HipScale}, {"adsScale", b.AdsScale}, {"ease", b.Ease},
    };
    const auto& br = Breath;
    j["breath"] = {
        {"enabled", br.Enabled}, {"period", br.Period}, {"position", Curve3Json(br.Position)},
        {"pitch", br.Pitch.ToJson()}, {"hipScale", br.HipScale}, {"adsScale", br.AdsScale},
    };
    j["aim"] = {{"position", Vec(Aim.Position)}, {"rotation", Vec(Aim.Rotation)},
                {"blendTime", Aim.BlendTime}, {"blend", Aim.Blend.ToJson()}};
    json offsets = json::array();
    for (const auto& o : StateOffsets)
        offsets.push_back({{"match", o.Match}, {"position", Vec(o.Position)}, {"rotation", Vec(o.Rotation)},
                           {"blendIn", o.BlendIn}, {"blendOut", o.BlendOut}});
    j["stateOffsets"] = offsets;
    const auto& l = Locomotion;
    j["locomotion"] = {{"matchSpeed", l.MatchSpeed}, {"walkReference", l.WalkReference},
                       {"sprintReference", l.SprintReference}, {"minRate", l.MinRate}, {"maxRate", l.MaxRate}};
    j["lean"] = {{"enabled", Lean.Enabled}, {"angle", Lean.Angle}, {"offset", Lean.Offset},
                 {"weaponRoll", Lean.WeaponRoll}, {"speed", Lean.Speed}};
    const auto& k = IK;
    j["ik"] = {{"enabled", k.Enabled}, {"gunBone", k.GunBone},
               {"rightUpper", k.RightUpper}, {"rightLower", k.RightLower}, {"rightHand", k.RightHand},
               {"leftUpper", k.LeftUpper}, {"leftLower", k.LeftLower}, {"leftHand", k.LeftHand},
               {"offTag", k.OffTag}, {"blendTime", k.BlendTime}};
    return j;
}

bool WeaponProceduralSettings::FromJson(const json& j, WeaponProceduralSettings& out, std::string* error) {
    if (!j.is_object()) {
        if (error) *error = "'procedural' must be an object";
        return false;
    }
    WeaponProceduralSettings s = out;
    Reader root{j, error};
    root.Object("recoil", [&](Reader& r) {
        auto& o = s.Recoil;
        r.Bool("enabled", o.Enabled);
        r.Number("duration", o.Duration);
        r.Curve3("rotation", o.Rotation);
        r.Curve3("position", o.Position);
        r.Vector("pitchRange", o.PitchRange);
        r.Vector("yawRange", o.YawRange);
        r.Vector("rollRange", o.RollRange);
        r.Vector("sideRange", o.SideRange);
        r.Vector("upRange", o.UpRange);
        r.Vector("kickRange", o.KickRange);
        r.Number("hipScale", o.HipScale);
        r.Number("adsScale", o.AdsScale);
        r.Vector("pivot", o.Pivot);
        r.Spring("smoothing", o.Smoothing);
        r.CurveField("cameraPitch", o.CameraPitch);
        r.CurveField("cameraYaw", o.CameraYaw);
        r.Vector("cameraYawRange", o.CameraYawRange);
        r.Number("cameraScale", o.CameraScale);
    });
    root.Object("sway", [&](Reader& r) {
        auto& o = s.Sway;
        r.Bool("enabled", o.Enabled);
        r.Number("lookRotation", o.LookRotation);
        r.Number("lookPosition", o.LookPosition);
        r.Number("maxRotation", o.MaxRotation);
        r.Number("maxPosition", o.MaxPosition);
        r.Number("movePosition", o.MovePosition);
        r.Number("moveRoll", o.MoveRoll);
        r.Number("adsScale", o.AdsScale);
        r.Spring("spring", o.Spring);
    });
    root.Object("bob", [&](Reader& r) {
        auto& o = s.Bob;
        r.Bool("enabled", o.Enabled);
        r.Number("walkStride", o.WalkStride);
        r.Number("sprintStride", o.SprintStride);
        r.Curve3("walk", o.Walk);
        r.Curve3("sprint", o.Sprint);
        r.Number("walkFullSpeed", o.WalkFullSpeed);
        r.Number("hipScale", o.HipScale);
        r.Number("adsScale", o.AdsScale);
        r.Number("ease", o.Ease);
    });
    root.Object("breath", [&](Reader& r) {
        auto& o = s.Breath;
        r.Bool("enabled", o.Enabled);
        r.Number("period", o.Period);
        r.Curve3("position", o.Position);
        r.CurveField("pitch", o.Pitch);
        r.Number("hipScale", o.HipScale);
        r.Number("adsScale", o.AdsScale);
    });
    root.Object("aim", [&](Reader& r) {
        r.Vector("position", s.Aim.Position);
        r.Vector("rotation", s.Aim.Rotation);
        r.Number("blendTime", s.Aim.BlendTime);
        r.CurveField("blend", s.Aim.Blend);
    });
    if (const auto it = j.find("stateOffsets"); it != j.end()) {
        if (!it->is_array()) {
            root.Fail("'stateOffsets' must be an array");
        } else {
            s.StateOffsets.clear();
            for (const json& e : *it) {
                if (!e.is_object()) { root.Fail("'stateOffsets' entries must be objects"); break; }
                WeaponStateOffset o;
                Reader r{e, error};
                r.String("match", o.Match);
                r.Vector("position", o.Position);
                r.Vector("rotation", o.Rotation);
                r.Number("blendIn", o.BlendIn);
                r.Number("blendOut", o.BlendOut);
                if (!r.Ok) { root.Ok = false; break; }
                s.StateOffsets.push_back(o);
            }
        }
    }
    root.Object("locomotion", [&](Reader& r) {
        auto& o = s.Locomotion;
        r.Bool("matchSpeed", o.MatchSpeed);
        r.Number("walkReference", o.WalkReference);
        r.Number("sprintReference", o.SprintReference);
        r.Number("minRate", o.MinRate);
        r.Number("maxRate", o.MaxRate);
    });
    root.Object("lean", [&](Reader& r) {
        r.Bool("enabled", s.Lean.Enabled);
        r.Number("angle", s.Lean.Angle);
        r.Number("offset", s.Lean.Offset);
        r.Number("weaponRoll", s.Lean.WeaponRoll);
        r.Number("speed", s.Lean.Speed);
    });
    root.Object("ik", [&](Reader& r) {
        auto& o = s.IK;
        r.Bool("enabled", o.Enabled);
        r.String("gunBone", o.GunBone);
        r.String("rightUpper", o.RightUpper);
        r.String("rightLower", o.RightLower);
        r.String("rightHand", o.RightHand);
        r.String("leftUpper", o.LeftUpper);
        r.String("leftLower", o.LeftLower);
        r.String("leftHand", o.LeftHand);
        r.String("offTag", o.OffTag);
        r.Number("blendTime", o.BlendTime);
    });
    if (!root.Ok) return false;

    const auto positive = [&](float v, const char* what) {
        if (v > 0.0f) return true;
        if (error) *error = std::string("'procedural.") + what + "' must be positive";
        return false;
    };
    if (!positive(s.Recoil.Duration, "recoil.duration") || !positive(s.Bob.WalkStride, "bob.walkStride") ||
        !positive(s.Bob.SprintStride, "bob.sprintStride") || !positive(s.Bob.WalkFullSpeed, "bob.walkFullSpeed") ||
        !positive(s.Breath.Period, "breath.period") || !positive(s.Locomotion.WalkReference, "locomotion.walkReference") ||
        !positive(s.Locomotion.SprintReference, "locomotion.sprintReference") ||
        !positive(s.Recoil.Smoothing.Frequency, "recoil.smoothing.frequency") ||
        !positive(s.Sway.Spring.Frequency, "sway.spring.frequency"))
        return false;
    out = std::move(s);
    return true;
}

// ---------------------------------------------------------------------------------------------

glm::quat WeaponProceduralPose::RotationQuat() const {
    const glm::vec3 r = glm::radians(Rotation);
    return glm::angleAxis(r.y, glm::vec3(0, 1, 0)) * glm::angleAxis(r.x, glm::vec3(1, 0, 0)) *
           glm::angleAxis(r.z, glm::vec3(0, 0, 1));
}

void WeaponProceduralState::Spring3::Step(const glm::vec3& target, float dt, const FirstPersonSpring& sp) {
    const float w = glm::two_pi<float>() * std::max(sp.Frequency, 0.01f);
    const float zeta = std::max(sp.Damping, 0.0f);
    // Semi-implicit Euler in sub-steps short enough to stay stable at any sane frequency.
    const int steps = std::clamp((int)std::ceil(dt * w / 0.5f), 1, 64);
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; ++i) {
        V += (w * w * (target - X) - 2.0f * zeta * w * V) * h;
        X += V * h;
    }
}

void WeaponProceduralState::Reset() {
    const auto rng = m_Rng;
    *this = WeaponProceduralState{};
    m_Rng = rng;
}

void WeaponProceduralState::OnShot(const WeaponProceduralSettings& s, bool ads) {
    const auto& r = s.Recoil;
    if (!r.Enabled) return;
    Shot shot;
    shot.Scale = ads ? r.AdsScale : r.HipScale;
    shot.Rot = {Pick(m_Rng, r.PitchRange), Pick(m_Rng, r.YawRange), Pick(m_Rng, r.RollRange)};
    shot.Pos = {Pick(m_Rng, r.SideRange), Pick(m_Rng, r.UpRange), Pick(m_Rng, r.KickRange)};
    shot.CamYaw = Pick(m_Rng, r.CameraYawRange);
    // A runaway trigger can't grow this without bound: the oldest shot has done its work.
    if (m_Shots.size() >= 32) m_Shots.erase(m_Shots.begin());
    m_Shots.push_back(shot);
}

void WeaponProceduralState::PreviewShot(const WeaponRecoilSettings& r, float seconds, glm::vec3& rotation,
                                        glm::vec3& position) {
    const float u = r.Duration > 0.0f ? seconds / r.Duration : 1.0f;
    rotation = r.Rotation.Evaluate(u);
    position = r.Position.Evaluate(u);
}

const WeaponProceduralPose& WeaponProceduralState::Update(const WeaponProceduralSettings& s,
                                                          const WeaponProceduralInput& in) {
    const float dt = std::clamp(std::isfinite(in.Dt) ? in.Dt : 0.0f, 0.0f, 0.1f);
    WeaponProceduralPose pose;

    // Blend weights.
    m_IK = MoveToward(m_IK, (in.IKOff || !s.IK.Enabled) ? 0.0f : 1.0f,
                      s.IK.BlendTime > 0.0f ? dt / s.IK.BlendTime : 1.0f);
    m_Ads = MoveToward(m_Ads, in.Ads ? 1.0f : 0.0f, s.Aim.BlendTime > 0.0f ? dt / s.Aim.BlendTime : 1.0f);
    const float ads = s.Aim.Blend.Empty() ? m_Ads : std::clamp(s.Aim.Blend.Evaluate(m_Ads), 0.0f, 1.0f);
    const auto adsMix = [ads](float hip, float aim) { return hip + (aim - hip) * ads; };
    pose.IKWeight = m_IK;

    // Recoil: every live shot's curves, summed, then smoothed by a spring so full-auto builds
    // into a climb and settles back instead of stepping.
    const auto& r = s.Recoil;
    glm::vec3 recoilRot(0.0f), recoilPos(0.0f);
    glm::vec2 camera(0.0f);
    for (Shot& shot : m_Shots) {
        shot.Time += dt;
        const float u = shot.Time / r.Duration;
        recoilRot += r.Rotation.Evaluate(u) * shot.Rot * shot.Scale;
        recoilPos += r.Position.Evaluate(u) * shot.Pos * shot.Scale;
        camera += glm::vec2(r.CameraPitch.Evaluate(u), r.CameraYaw.Evaluate(u) * shot.CamYaw) * shot.Scale * r.CameraScale;
    }
    m_Shots.erase(std::remove_if(m_Shots.begin(), m_Shots.end(), [&](const Shot& sh) { return sh.Time >= r.Duration; }),
                  m_Shots.end());
    if (!r.Enabled) {
        recoilRot = recoilPos = glm::vec3(0.0f);
        camera = glm::vec2(0.0f);
    }
    m_RecoilRot.Step(recoilRot, dt, r.Smoothing);
    m_RecoilPos.Step(recoilPos, dt, r.Smoothing);
    pose.Rotation += m_RecoilRot.X;
    pose.Position += m_RecoilPos.X;
    pose.CameraKick = camera;
    pose.Pivot = r.Pivot;

    // Sway: the gun lags behind a turn and trails against movement.
    const auto& w = s.Sway;
    glm::vec3 swayRot(0.0f), swayPos(0.0f);
    if (w.Enabled) {
        const glm::vec2 look = in.LookRate / 100.0f;
        swayRot = glm::vec3(-look.y, look.x, -0.5f * look.x) * w.LookRotation;
        swayPos = glm::vec3(-look.x, -look.y, 0.0f) * w.LookPosition;
        swayPos += glm::vec3(-in.Velocity.x, 0.0f, -in.Velocity.z) * w.MovePosition;
        swayRot.z += -in.Velocity.x * w.MoveRoll;
        swayRot = glm::clamp(swayRot, glm::vec3(-w.MaxRotation), glm::vec3(w.MaxRotation));
        swayPos = ClampLength(swayPos, w.MaxPosition);
        const float scale = adsMix(1.0f, w.AdsScale);
        swayRot *= scale;
        swayPos *= scale;
    }
    m_SwayRot.Step(swayRot, dt, w.Spring);
    m_SwayPos.Step(swayPos, dt, w.Spring);
    pose.Rotation += m_SwayRot.X;
    pose.Position += m_SwayPos.X;

    // Bob: phase-locked to distance travelled, so it never slides against the footsteps.
    const auto& b = s.Bob;
    const float speed = glm::length(glm::vec2(in.Velocity.x, in.Velocity.z));
    const float ease = std::min(1.0f, dt * std::max(b.Ease, 0.0f));
    m_BobWeight += ((b.Enabled ? std::min(speed / b.WalkFullSpeed, 1.0f) : 0.0f) - m_BobWeight) * ease;
    m_BobSprint += ((in.Sprinting ? 1.0f : 0.0f) - m_BobSprint) * ease;
    const float stride = b.WalkStride + (b.SprintStride - b.WalkStride) * m_BobSprint;
    m_BobPhase = std::fmod(m_BobPhase + speed * dt / stride, 1.0f);
    const glm::vec3 bob = (b.Walk.Evaluate(m_BobPhase) * (1.0f - m_BobSprint) + b.Sprint.Evaluate(m_BobPhase) * m_BobSprint) *
                          m_BobWeight * adsMix(b.HipScale, b.AdsScale);
    pose.Position += glm::vec3(bob.x, bob.y, 0.0f);
    pose.Rotation.z += bob.z;

    // Breathing: a slow idle drift, calmer with sights up.
    const auto& br = s.Breath;
    m_BreathPhase = std::fmod(m_BreathPhase + dt / br.Period, 1.0f);
    if (br.Enabled) {
        const float scale = adsMix(br.HipScale, br.AdsScale);
        pose.Position += br.Position.Evaluate(m_BreathPhase) * scale;
        pose.Rotation.x += br.Pitch.Evaluate(m_BreathPhase) * scale;
    }

    // ADS aim offset.
    pose.Position += s.Aim.Position * ads;
    pose.Rotation += s.Aim.Rotation * ads;

    // Per-state pose offsets.
    m_StateWeights.resize(s.StateOffsets.size(), 0.0f);
    for (size_t i = 0; i < s.StateOffsets.size(); ++i) {
        const auto& o = s.StateOffsets[i];
        bool match = false;
        if (!o.Match.empty()) {
            if (in.StateName && *in.StateName == o.Match) match = true;
            if (in.StateTags)
                for (const auto& t : *in.StateTags) match = match || t == o.Match;
        }
        const float time = match ? o.BlendIn : o.BlendOut;
        m_StateWeights[i] = MoveToward(m_StateWeights[i], match ? 1.0f : 0.0f, time > 0.0f ? dt / time : 1.0f);
        const float weight = Smooth01(m_StateWeights[i]);
        pose.Position += o.Position * weight;
        pose.Rotation += o.Rotation * weight;
    }

    // Lean: the camera rolls and slides; the gun rolls a little further into it.
    const auto& l = s.Lean;
    m_Lean += ((l.Enabled ? std::clamp(in.Lean, -1.0f, 1.0f) : 0.0f) - m_Lean) * std::min(1.0f, dt * std::max(l.Speed, 0.0f));
    pose.CameraRoll = m_Lean * l.Angle;
    pose.CameraSide = m_Lean * l.Offset;
    pose.Rotation.z += -m_Lean * l.WeaponRoll;

    // Locomotion clip rates.
    const auto& lo = s.Locomotion;
    if (lo.MatchSpeed && speed > 0.05f) {
        pose.WalkRate = std::clamp(speed / lo.WalkReference, lo.MinRate, lo.MaxRate);
        pose.SprintRate = std::clamp(speed / lo.SprintReference, lo.MinRate, lo.MaxRate);
    }

    m_Pose = pose;
    return m_Pose;
}
