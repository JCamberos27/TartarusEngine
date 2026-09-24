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

float Gauss(std::mt19937& rng, float mean, float sigma) {
    return sigma > 0.0f ? std::normal_distribution<float>(mean, sigma)(rng) : mean;
}

// Smooth 1D value noise in [-1, 1]: hashed lattice values, quintic-eased between them.
float Noise1(float x, unsigned channel) {
    auto lattice = [channel](int i) {
        unsigned h = (unsigned)i * 374761393u + channel * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return (float)(h & 0xffffu) / 32767.5f - 1.0f;
    };
    const float f = std::floor(x);
    const float t = x - f;
    const float e = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    const int i = (int)f;
    return lattice(i) + (lattice(i + 1) - lattice(i)) * e;
}

float Smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float MoveToward(float from, float to, float step) {
    if (from < to) return std::min(to, from + step);
    return std::max(to, from - step);
}

// Eases into +-limit instead of stopping dead at it: ~linear well below, never past it.
float SoftLimit(float v, float limit) { return limit > 0.0f ? limit * std::tanh(v / limit) : 0.0f; }
glm::vec3 SoftLimit(const glm::vec3& v, float limit) { return {SoftLimit(v.x, limit), SoftLimit(v.y, limit), SoftLimit(v.z, limit)}; }
glm::vec3 SoftLimitLength(const glm::vec3& v, float limit) {
    const float len = glm::length(v);
    return len > 1e-9f ? v * (SoftLimit(len, limit) / len) : v;
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
        {"kickSpread", r.KickSpread}, {"kickBias", r.KickBias}, {"timeJitter", r.TimeJitter},
        {"firstShotScale", r.FirstShotScale}, {"wander", r.Wander}, {"wanderReturn", r.WanderReturn},
        {"burstGrowth", r.BurstGrowth}, {"burstGrowthMax", r.BurstGrowthMax},
        {"hipScale", r.HipScale}, {"adsScale", r.AdsScale}, {"pivot", Vec(r.Pivot)},
        {"smoothing", SpringJson(r.Smoothing)},
        {"cameraPitch", r.CameraPitch.ToJson()}, {"cameraYaw", r.CameraYaw.ToJson()},
        {"cameraYawRange", Vec(r.CameraYawRange)}, {"cameraScale", r.CameraScale},
        {"cameraSmoothing", SpringJson(r.CameraSmoothing)},
        {"aimPitch", Vec(r.AimPitch)}, {"aimYaw", Vec(r.AimYaw)}, {"aimRecovery", r.AimRecovery},
        {"aimRecoveryDelay", r.AimRecoveryDelay}, {"aimRecoverySpeed", r.AimRecoverySpeed},
        {"shakeAmount", r.ShakeAmount}, {"shakeMax", Vec(r.ShakeMax)}, {"shakeFrequency", r.ShakeFrequency},
        {"shakeDecay", r.ShakeDecay}, {"shakeAdsScale", r.ShakeAdsScale},
        {"hipProcedural", r.HipProcedural}, {"boltCycle", r.BoltCycle}, {"boltBone", r.BoltBone},
        {"cameraRoll", r.CameraRoll}, {"fovPunch", r.FovPunch}, {"punchSpring", SpringJson(r.PunchSpring)},
    };
    const auto& w = Sway;
    j["sway"] = {
        {"enabled", w.Enabled}, {"lookRotation", w.LookRotation}, {"lookPosition", w.LookPosition},
        {"maxRotation", w.MaxRotation}, {"maxPosition", w.MaxPosition},
        {"movePosition", w.MovePosition}, {"moveRoll", w.MoveRoll}, {"adsScale", w.AdsScale},
        {"spring", SpringJson(w.Spring)}, {"lookSmoothing", w.LookSmoothing},
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
        {"driftPosition", Vec(br.DriftPosition)}, {"driftRotation", Vec(br.DriftRotation)},
        {"driftFrequency", br.DriftFrequency}, {"exertionScale", br.ExertionScale}, {"exertionRate", br.ExertionRate},
        {"exertionBuild", br.ExertionBuild}, {"exertionRecover", br.ExertionRecover},
    };
    const auto& jp = Jump;
    j["jump"] = {
        {"enabled", jp.Enabled}, {"airPosition", jp.AirPosition}, {"airPitch", jp.AirPitch},
        {"maxPosition", jp.MaxPosition}, {"maxPitch", jp.MaxPitch}, {"landPosition", jp.LandPosition},
        {"landPitch", jp.LandPitch}, {"landRoll", jp.LandRoll}, {"minImpact", jp.MinImpact},
        {"maxImpact", jp.MaxImpact}, {"adsScale", jp.AdsScale}, {"spring", SpringJson(jp.Spring)},
    };
    const auto& cm = CameraMotion;
    j["cameraMotion"] = {
        {"enabled", cm.Enabled}, {"walkBob", Vec(cm.WalkBob)}, {"sprintBob", Vec(cm.SprintBob)},
        {"strafeRoll", cm.StrafeRoll}, {"landDip", cm.LandDip}, {"landPitch", cm.LandPitch},
        {"adsScale", cm.AdsScale}, {"spring", SpringJson(cm.Spring)},
    };
    const auto& ob = Obstruction;
    j["obstruction"] = {
        {"enabled", ob.Enabled}, {"reach", ob.Reach}, {"maxRetract", ob.MaxRetract}, {"tuckRange", ob.TuckRange},
        {"probeOffset", Vec(ob.ProbeOffset)}, {"position", Vec(ob.Position)}, {"rotation", Vec(ob.Rotation)},
        {"highPosition", Vec(ob.HighPosition)}, {"highRotation", Vec(ob.HighRotation)},
        {"sidePosition", Vec(ob.SidePosition)}, {"sideRotation", Vec(ob.SideRotation)}, {"blockAt", ob.BlockAt},
        {"spring", SpringJson(ob.Spring)},
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
                 {"weaponRoll", Lean.WeaponRoll}, {"weaponRollAds", Lean.WeaponRollAds},
                 {"spring", SpringJson(Lean.Spring)}, {"weaponSpring", SpringJson(Lean.WeaponSpring)},
                 {"whileSprinting", Lean.WhileSprinting}, {"cornerPeek", Lean.CornerPeek},
                 {"peekRange", Lean.PeekRange}, {"peekMargin", Lean.PeekMargin}};
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
        r.Number("kickSpread", o.KickSpread);
        r.Number("kickBias", o.KickBias);
        r.Number("timeJitter", o.TimeJitter);
        r.Number("firstShotScale", o.FirstShotScale);
        r.Number("wander", o.Wander);
        r.Number("wanderReturn", o.WanderReturn);
        r.Number("burstGrowth", o.BurstGrowth);
        r.Number("burstGrowthMax", o.BurstGrowthMax);
        r.Number("hipScale", o.HipScale);
        r.Number("adsScale", o.AdsScale);
        r.Vector("pivot", o.Pivot);
        r.Spring("smoothing", o.Smoothing);
        r.CurveField("cameraPitch", o.CameraPitch);
        r.CurveField("cameraYaw", o.CameraYaw);
        r.Vector("cameraYawRange", o.CameraYawRange);
        r.Number("cameraScale", o.CameraScale);
        r.Spring("cameraSmoothing", o.CameraSmoothing);
        r.Vector("aimPitch", o.AimPitch);
        r.Vector("aimYaw", o.AimYaw);
        r.Number("aimRecovery", o.AimRecovery);
        r.Number("aimRecoveryDelay", o.AimRecoveryDelay);
        r.Number("aimRecoverySpeed", o.AimRecoverySpeed);
        r.Number("shakeAmount", o.ShakeAmount);
        r.Vector("shakeMax", o.ShakeMax);
        r.Number("shakeFrequency", o.ShakeFrequency);
        r.Number("shakeDecay", o.ShakeDecay);
        r.Number("shakeAdsScale", o.ShakeAdsScale);
        r.Bool("hipProcedural", o.HipProcedural);
        r.Number("boltCycle", o.BoltCycle);
        r.String("boltBone", o.BoltBone);
        r.Number("cameraRoll", o.CameraRoll);
        r.Number("fovPunch", o.FovPunch);
        r.Spring("punchSpring", o.PunchSpring);
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
        r.Number("lookSmoothing", o.LookSmoothing);
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
        r.Vector("driftPosition", o.DriftPosition);
        r.Vector("driftRotation", o.DriftRotation);
        r.Number("driftFrequency", o.DriftFrequency);
        r.Number("exertionScale", o.ExertionScale);
        r.Number("exertionRate", o.ExertionRate);
        r.Number("exertionBuild", o.ExertionBuild);
        r.Number("exertionRecover", o.ExertionRecover);
    });
    root.Object("jump", [&](Reader& r) {
        auto& o = s.Jump;
        r.Bool("enabled", o.Enabled);
        r.Number("airPosition", o.AirPosition);
        r.Number("airPitch", o.AirPitch);
        r.Number("maxPosition", o.MaxPosition);
        r.Number("maxPitch", o.MaxPitch);
        r.Number("landPosition", o.LandPosition);
        r.Number("landPitch", o.LandPitch);
        r.Number("landRoll", o.LandRoll);
        r.Number("minImpact", o.MinImpact);
        r.Number("maxImpact", o.MaxImpact);
        r.Number("adsScale", o.AdsScale);
        r.Spring("spring", o.Spring);
    });
    root.Object("cameraMotion", [&](Reader& r) {
        auto& o = s.CameraMotion;
        r.Bool("enabled", o.Enabled);
        r.Vector("walkBob", o.WalkBob);
        r.Vector("sprintBob", o.SprintBob);
        r.Number("strafeRoll", o.StrafeRoll);
        r.Number("landDip", o.LandDip);
        r.Number("landPitch", o.LandPitch);
        r.Number("adsScale", o.AdsScale);
        r.Spring("spring", o.Spring);
    });
    root.Object("obstruction", [&](Reader& r) {
        auto& o = s.Obstruction;
        r.Bool("enabled", o.Enabled);
        r.Number("reach", o.Reach);
        r.Number("maxRetract", o.MaxRetract);
        r.Number("tuckRange", o.TuckRange);
        r.Vector("probeOffset", o.ProbeOffset);
        r.Vector("position", o.Position);
        r.Vector("rotation", o.Rotation);
        r.Vector("highPosition", o.HighPosition);
        r.Vector("highRotation", o.HighRotation);
        r.Vector("sidePosition", o.SidePosition);
        r.Vector("sideRotation", o.SideRotation);
        r.Number("blockAt", o.BlockAt);
        r.Spring("spring", o.Spring);
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
        r.Number("weaponRollAds", s.Lean.WeaponRollAds);
        r.Spring("spring", s.Lean.Spring);
        r.Spring("weaponSpring", s.Lean.WeaponSpring);
        r.Bool("whileSprinting", s.Lean.WhileSprinting);
        r.Bool("cornerPeek", s.Lean.CornerPeek);
        r.Number("peekRange", s.Lean.PeekRange);
        r.Number("peekMargin", s.Lean.PeekMargin);
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
        !positive(s.Breath.Period, "breath.period") ||
        !positive(s.Recoil.Smoothing.Frequency, "recoil.smoothing.frequency") ||
        !positive(s.Sway.Spring.Frequency, "sway.spring.frequency"))
        return false;
    if (!(s.Locomotion.WalkReference >= 0.0f) || !(s.Locomotion.SprintReference >= 0.0f)) {
        if (error) *error = "'procedural.locomotion' references must be 0 (the player's speed) or positive";
        return false;
    }
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

void WeaponProceduralState::OnShot(const WeaponProceduralSettings& s, bool ads, bool cycleBolt) {
    const auto& r = s.Recoil;
    if (!r.Enabled) return;
    // A pause longer than a couple of cycles at any sane rate starts a new burst.
    m_BurstShots = m_SinceShot > 0.25f ? 0 : m_BurstShots + 1;
    if (m_BurstShots == 0) m_Wander = 0.0f;
    const float growth = std::clamp(1.0f + r.BurstGrowth * (float)m_BurstShots, 1.0f, std::max(1.0f, r.BurstGrowthMax));
    Shot shot;
    shot.Scale = (ads ? r.AdsScale : r.HipScale) * (m_BurstShots == 0 ? r.FirstShotScale : 1.0f);
    const float jitter = std::clamp(r.TimeJitter, 0.0f, 0.9f);
    shot.Duration = Pick(m_Rng, {1.0f - jitter, 1.0f + jitter});
    shot.Rot = {Pick(m_Rng, r.PitchRange), Pick(m_Rng, r.YawRange), Pick(m_Rng, r.RollRange)};
    shot.Pos = {Pick(m_Rng, r.SideRange), Pick(m_Rng, r.UpRange), Pick(m_Rng, r.KickRange)};
    // The kick's direction: part of the rise swings sideways, and the camera punch follows it.
    const float angle = glm::radians(std::clamp(Gauss(m_Rng, r.KickBias, r.KickSpread * growth), -75.0f, 75.0f));
    shot.Lift = shot.Rot.x * std::sin(angle);
    shot.Rot.x *= std::cos(angle);
    shot.CamPitch = shot.Rot.x;
    shot.CamYaw = Pick(m_Rng, r.CameraYawRange) + std::sin(angle) * 2.0f;
    // Sideways climb: a random walk pulled back toward centre, so the burst meanders.
    m_Wander = m_Wander * (1.0f - std::clamp(r.WanderReturn, 0.0f, 1.0f)) + Gauss(m_Rng, 0.0f, r.Wander * growth);
    const glm::vec2 climb(Pick(m_Rng, r.AimPitch), Pick(m_Rng, r.AimYaw) + m_Wander);
    m_AimPending += climb * shot.Scale;
    m_AimRecoverable += climb * shot.Scale * std::clamp(r.AimRecovery, 0.0f, 1.0f);
    m_SinceShot = 0.0f;
    m_Trauma = std::min(1.0f, m_Trauma + std::max(r.ShakeAmount, 0.0f));
    // The camera's per-round roll and FOV pulse: a kick of the punch spring's velocity, sized so
    // the spring's first swing peaks near the amount.
    {
        const float w = glm::two_pi<float>() * std::max(r.PunchSpring.Frequency, 0.01f);
        const float scale = shot.Scale * (ads ? r.ShakeAdsScale : 1.0f) * w * 1.6f;
        const float side = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng) < 0.5f ? -1.0f : 1.0f;
        m_Punch.V.x += side * Pick(m_Rng, {0.5f, 1.0f}) * r.CameraRoll * scale;
        m_Punch.V.y += r.FovPunch * scale;
    }
    if (cycleBolt) m_BoltTime = 0.0f;
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
    m_IK = MoveToward(m_IK, in.IKOff ? 0.0f : 1.0f,
                      s.IK.BlendTime > 0.0f ? dt / s.IK.BlendTime : 1.0f);
    m_Ads = MoveToward(m_Ads, in.Ads ? 1.0f : 0.0f, s.Aim.BlendTime > 0.0f ? dt / s.Aim.BlendTime : 1.0f);
    const float ads = s.Aim.Blend.Empty() ? m_Ads : std::clamp(s.Aim.Blend.Evaluate(m_Ads), 0.0f, 1.0f);
    const auto adsMix = [ads](float hip, float aim) { return hip + (aim - hip) * ads; };
    pose.IKWeight = m_IK;

    // Recoil: every live shot's curves, summed, then smoothed by a spring so full-auto builds
    // into a climb and settles back instead of stepping.
    const auto& r = s.Recoil;
    // Guarded here as well as in FromJson: the Inspector edits these live.
    const float duration = std::max(r.Duration, 0.01f);
    glm::vec3 recoilRot(0.0f), recoilPos(0.0f);
    glm::vec2 camera(0.0f);
    for (Shot& shot : m_Shots) {
        shot.Time += dt;
        const float u = shot.Time / (duration * shot.Duration);
        const glm::vec3 rot = r.Rotation.Evaluate(u);
        recoilRot += (rot * shot.Rot + glm::vec3(0.0f, rot.x * shot.Lift, 0.0f)) * shot.Scale;
        recoilPos += r.Position.Evaluate(u) * shot.Pos * shot.Scale;
        camera += glm::vec2(r.CameraPitch.Evaluate(u) * shot.CamPitch, r.CameraYaw.Evaluate(u) * shot.CamYaw) *
                  shot.Scale * r.CameraScale;
    }
    m_Shots.erase(std::remove_if(m_Shots.begin(), m_Shots.end(),
                                 [&](const Shot& sh) { return sh.Time >= duration * sh.Duration; }),
                  m_Shots.end());
    if (!r.Enabled) {
        recoilRot = recoilPos = glm::vec3(0.0f);
        camera = glm::vec2(0.0f);
    }
    m_RecoilRot.Step(recoilRot, dt, r.Smoothing);
    m_RecoilPos.Step(recoilPos, dt, r.Smoothing);
    pose.Rotation += m_RecoilRot.X;
    pose.Position += m_RecoilPos.X;
    if (r.CameraSmoothing.Frequency > 0.0f) {
        m_Camera.Step(glm::vec3(camera, 0.0f), dt, r.CameraSmoothing);
        camera = glm::vec2(m_Camera.X);
    }
    // Shake: smooth noise on each axis, sized by trauma squared, laid over the punch.
    m_Trauma = std::max(0.0f, m_Trauma - std::max(r.ShakeDecay, 0.0f) * dt);
    m_ShakeTime = std::fmod(m_ShakeTime + dt * std::max(r.ShakeFrequency, 0.0f), 4096.0f);
    if (r.Enabled && m_Trauma > 0.0f) {
        const float amount = m_Trauma * m_Trauma * adsMix(1.0f, r.ShakeAdsScale);
        camera.x += amount * r.ShakeMax.x * Noise1(m_ShakeTime, 1u);
        camera.y += amount * r.ShakeMax.y * Noise1(m_ShakeTime, 2u);
        pose.CameraRoll += amount * r.ShakeMax.z * Noise1(m_ShakeTime, 3u);
    }
    m_Punch.Step(glm::vec3(0.0f), dt, r.PunchSpring);
    if (r.Enabled) {
        pose.CameraRoll += m_Punch.X.x;
        pose.FovKick += m_Punch.X.y;
    }
    pose.CameraKick = camera;
    pose.Pivot = r.Pivot;

    // Aim climb: fed in over ~30 ms (a round's impulse, not a one-frame snap), then - once the
    // trigger rests - the recoverable share eases back.
    {
        const float feed = std::min(1.0f, dt / 0.03f);
        glm::vec2 kick = m_AimPending * feed;
        m_AimPending -= kick;
        m_SinceShot += dt;
        if (m_SinceShot >= r.AimRecoveryDelay && r.AimRecoverySpeed > 0.0f) {
            const float len = glm::length(m_AimRecoverable);
            if (len > 1e-6f) {
                const float step = std::min(len, r.AimRecoverySpeed * dt);
                const glm::vec2 back = m_AimRecoverable * (step / len);
                m_AimRecoverable -= back;
                kick -= back;
            }
        }
        pose.AimKick = r.Enabled ? kick : glm::vec2(0.0f);
    }
    // Bolt: slams back over the first 35% of its cycle (easing out), returns over the rest.
    m_BoltTime += dt;
    if (r.Enabled && r.BoltCycle > 0.0f && m_BoltTime < r.BoltCycle) {
        const float u = m_BoltTime / r.BoltCycle;
        pose.Bolt = u < 0.35f ? std::sin(glm::half_pi<float>() * u / 0.35f)
                              : 0.5f + 0.5f * std::cos(glm::pi<float>() * (u - 0.35f) / 0.65f);
    }

    // Sway: the gun lags behind a turn and trails against movement.
    const auto& w = s.Sway;
    glm::vec3 swayRot(0.0f), swayPos(0.0f);
    // Mouse input is ragged frame to frame: low-pass the rate so the gun glides instead of buzzing.
    m_Look = w.LookSmoothing > 0.0f
                 ? m_Look + (in.LookRate - m_Look) * (1.0f - std::exp(-glm::two_pi<float>() * w.LookSmoothing * dt))
                 : in.LookRate;
    if (w.Enabled) {
        const glm::vec2 look = m_Look / 100.0f;
        swayRot = glm::vec3(-look.y, look.x, -0.5f * look.x) * w.LookRotation;
        swayPos = glm::vec3(-look.x, -look.y, 0.0f) * w.LookPosition;
        swayPos += glm::vec3(-in.Velocity.x, 0.0f, -in.Velocity.z) * w.MovePosition;
        swayRot.z += -in.Velocity.x * w.MoveRoll;
        swayRot = SoftLimit(swayRot, w.MaxRotation);
        swayPos = SoftLimitLength(swayPos, w.MaxPosition);
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
    m_BobWeight += ((b.Enabled ? std::min(speed / std::max(b.WalkFullSpeed, 0.01f), 1.0f) : 0.0f) - m_BobWeight) * ease;
    m_BobSprint += ((in.Sprinting ? 1.0f : 0.0f) - m_BobSprint) * ease;
    const float stride = std::max(b.WalkStride + (b.SprintStride - b.WalkStride) * m_BobSprint, 0.01f);
    m_BobPhase = std::fmod(m_BobPhase + speed * dt / stride, 1.0f);
    const glm::vec3 bob = (b.Walk.Evaluate(m_BobPhase) * (1.0f - m_BobSprint) + b.Sprint.Evaluate(m_BobPhase) * m_BobSprint) *
                          m_BobWeight * adsMix(b.HipScale, b.AdsScale);
    pose.Position += glm::vec3(bob.x, bob.y, 0.0f);
    pose.Rotation.z += bob.z;

    // Breathing: a slow idle drift, calmer with sights up.
    const auto& br = s.Breath;
    // Sprinting winds the player; it builds while sprinting and fades once they stop.
    m_Exertion = in.Sprinting ? std::min(1.0f, m_Exertion + (br.ExertionBuild > 0.0f ? dt / br.ExertionBuild : 1.0f))
                              : std::max(0.0f, m_Exertion - (br.ExertionRecover > 0.0f ? dt / br.ExertionRecover : 1.0f));
    const float winded = Smooth01(m_Exertion);
    const float rate = 1.0f + (std::max(br.ExertionRate, 0.0f) - 1.0f) * winded;
    m_BreathPhase = std::fmod(m_BreathPhase + dt * rate / std::max(br.Period, 0.05f), 1.0f);
    m_DriftTime = std::fmod(m_DriftTime + dt * rate * std::max(br.DriftFrequency, 0.0f), 4096.0f);
    if (br.Enabled) {
        const float scale = adsMix(br.HipScale, br.AdsScale) * (1.0f + (std::max(br.ExertionScale, 0.0f) - 1.0f) * winded);
        pose.Position += br.Position.Evaluate(m_BreathPhase) * scale;
        pose.Rotation.x += br.Pitch.Evaluate(m_BreathPhase) * scale;
        // Two octaves of noise per axis: a slow wander with a little faster texture on it.
        auto drift = [&](unsigned ch) { return 0.75f * Noise1(m_DriftTime, ch) + 0.25f * Noise1(m_DriftTime * 2.7f, ch + 17u); };
        pose.Position += glm::vec3(drift(11u), drift(12u), drift(13u)) * br.DriftPosition * scale;
        pose.Rotation += glm::vec3(drift(14u), drift(15u), drift(16u)) * br.DriftRotation * scale;
    }

    // Jump and land: the gun lags against vertical speed in the air, and a landing kicks it
    // (and the camera) down by the fall speed.
    const auto& jp = s.Jump;
    const auto& cm = s.CameraMotion;
    float impact = 0.0f;
    if (in.Grounded && !m_WasGrounded) impact = std::max(0.0f, -m_AirVy);
    if (!in.Grounded) m_AirVy = in.VerticalVelocity;
    m_WasGrounded = in.Grounded;
    {
        const float jumpScale = adsMix(1.0f, jp.AdsScale);
        glm::vec3 airPos(0.0f), airRot(0.0f);
        if (jp.Enabled && !in.Grounded) {
            airPos.y = SoftLimit(-in.VerticalVelocity * jp.AirPosition, jp.MaxPosition);
            airRot.x = SoftLimit(-in.VerticalVelocity * jp.AirPitch, jp.MaxPitch);
        }
        const float hit = impact >= jp.MinImpact ? std::min(impact, std::max(jp.MaxImpact, jp.MinImpact)) : 0.0f;
        if (jp.Enabled && hit > 0.0f) {
            const float kick = glm::two_pi<float>() * std::max(jp.Spring.Frequency, 0.01f) * 1.6f;
            const float side = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng) < 0.5f ? -1.0f : 1.0f;
            m_JumpPos.V.y -= hit * jp.LandPosition * kick * jumpScale;
            m_JumpRot.V.x -= hit * jp.LandPitch * kick * jumpScale;
            m_JumpRot.V.z += side * hit * jp.LandRoll * kick * jumpScale;
        }
        m_JumpPos.Step(airPos * jumpScale, dt, jp.Spring);
        m_JumpRot.Step(airRot * jumpScale, dt, jp.Spring);
        pose.Position += m_JumpPos.X;
        pose.Rotation += m_JumpRot.X;
    }

    // The camera: head bob on the gun bob's stride, a roll into strafes, a dip on landing.
    if (cm.Enabled) {
        const float camScale = adsMix(1.0f, cm.AdsScale);
        const glm::vec2 amp = (cm.WalkBob * (1.0f - m_BobSprint) + cm.SprintBob * m_BobSprint) * m_BobWeight * camScale;
        const float tau = glm::two_pi<float>();
        pose.CameraOffset.y -= amp.x * 0.5f * (1.0f - std::cos(2.0f * tau * m_BobPhase)); // down on each footfall
        pose.CameraRoll += amp.y * std::sin(tau * m_BobPhase);
        if (impact >= s.Jump.MinImpact) {
            const float hit = std::min(impact, std::max(s.Jump.MaxImpact, s.Jump.MinImpact));
            const float kick = glm::two_pi<float>() * std::max(cm.Spring.Frequency, 0.01f) * 1.6f;
            m_CamMotion.V.y -= hit * cm.LandDip * kick * camScale;
            m_CamMotion.V.x -= hit * cm.LandPitch * kick * camScale;
        }
        const float strafe = in.Grounded ? in.Velocity.x * cm.StrafeRoll * camScale : 0.0f;
        m_CamMotion.Step(glm::vec3(0.0f, 0.0f, -strafe), dt, cm.Spring);
        pose.CameraOffset.y += m_CamMotion.X.y;
        pose.CameraKick.x += m_CamMotion.X.x;
        pose.CameraRoll += m_CamMotion.X.z;
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

    // Walls: first slide the gun straight back as far as the wall is in (the muzzle stops at
    // the surface, still aimed), then tuck it - down, or up off a surface that faces up.
    {
        const auto& ob = s.Obstruction;
        float in_ = 0.0f;
        if (ob.Enabled && ob.Reach > 0.0f && in.WallDistance >= 0.0f) in_ = std::max(0.0f, ob.Reach - in.WallDistance);
        const float maxRetract = std::max(ob.MaxRetract, 0.0f);
        const float retract = std::min(in_, maxRetract);
        const float tuck = ob.TuckRange > 0.0f ? Smooth01((in_ - maxRetract) / ob.TuckRange) : (in_ > maxRetract ? 1.0f : 0.0f);
        // Which way to tuck only changes while tucking, so leaving a table or a corner doesn't
        // flip it mid-way. An edge beside the barrel (its surface facing sideways) pushes the gun
        // aside, away from it: -1 = it's on the left, +1 = on the right.
        const float up = tuck > 0.0f ? (in.WallFacesUp ? 1.0f : 0.0f) : m_Obstruct.X.z;
        const float sideways = std::clamp((std::fabs(in.WallSide) - 0.35f) / 0.35f, 0.0f, 1.0f);
        const float aside = tuck > 0.0f ? (in.WallSide < 0.0f ? sideways : -sideways) : m_ObstructSide.X.x;
        m_Obstruct.Step(glm::vec3(retract, tuck, up), dt, ob.Spring);
        m_ObstructSide.Step(glm::vec3(aside, 0.0f, 0.0f), dt, ob.Spring);
        const float back = std::clamp(m_Obstruct.X.x, 0.0f, maxRetract);
        const float k = std::clamp(m_Obstruct.X.y, 0.0f, 1.0f);
        const float high = std::clamp(m_Obstruct.X.z, 0.0f, 1.0f);
        const float side = std::clamp(m_ObstructSide.X.x, -1.0f, 1.0f);
        // Low or high ready, then aside by how sideways the surface faces; the side pose is for
        // an obstruction on the right, mirrored (x, yaw, roll) for the left.
        const float as = std::fabs(side), sign = side < 0.0f ? -1.0f : 1.0f;
        const glm::vec3 mirror(sign, 1.0f, sign);
        const glm::vec3 tuckPos = ob.Position + (ob.HighPosition - ob.Position) * high;
        const glm::vec3 tuckRot = ob.Rotation + (ob.HighRotation - ob.Rotation) * high;
        const glm::vec3 sideRot(ob.SideRotation.x, ob.SideRotation.y * sign, ob.SideRotation.z * sign);
        pose.Obstruction = k;
        pose.Position.z += back;
        pose.Position += (tuckPos + (ob.SidePosition * mirror - tuckPos) * as) * k;
        pose.Rotation += (tuckRot + (sideRot - tuckRot) * as) * k;
    }

    // Lean: the head swings over on an arc about the waist (rolling, sliding and dropping a
    // little); the gun rolls a little further into it, lagging on its own spring.
    const auto& l = s.Lean;
    const bool canLean = l.Enabled && (l.WhileSprinting || !in.Sprinting);
    const float leanTarget = canLean ? std::clamp(in.Lean, -1.0f, 1.0f) : 0.0f;
    {
        glm::vec3 head(m_Lean.X.x, 0.0f, 0.0f), headV(m_Lean.V.x, 0.0f, 0.0f);
        glm::vec3 gun(m_Lean.X.y, 0.0f, 0.0f), gunV(m_Lean.V.y, 0.0f, 0.0f);
        Spring3 a{head, headV}, g{gun, gunV};
        a.Step(glm::vec3(leanTarget, 0.0f, 0.0f), dt, l.Spring);
        g.Step(glm::vec3(leanTarget, 0.0f, 0.0f), dt, l.WeaponSpring);
        m_Lean.X = glm::vec3(a.X.x, g.X.x, 0.0f);
        m_Lean.V = glm::vec3(a.V.x, g.V.x, 0.0f);
    }
    const float lean = std::clamp(m_Lean.X.x, -1.2f, 1.2f); // a spring may overshoot a touch
    const float angle = glm::radians(l.Angle);
    const float theta = lean * angle;
    if (std::fabs(std::sin(angle)) > 1e-4f) {
        const float radius = l.Offset / std::sin(angle); // the pivot below the eye that gives Offset at Angle
        pose.CameraSide = radius * std::sin(theta);
        pose.CameraOffset.y -= radius * (1.0f - std::cos(theta));
    } else {
        pose.CameraSide = lean * l.Offset;
    }
    pose.CameraRoll += lean * l.Angle;
    pose.Rotation.z += -m_Lean.X.y * l.WeaponRoll * adsMix(1.0f, l.WeaponRollAds);

    // Locomotion clip rates.
    const auto& lo = s.Locomotion;
    const float walkRef = lo.WalkReference > 0.0f ? lo.WalkReference : in.WalkSpeed;
    const float sprintRef = lo.SprintReference > 0.0f ? lo.SprintReference : in.SprintSpeed;
    // std::clamp needs lo <= hi: a Min above Max must not be undefined behaviour.
    const float minRate = std::max(0.0f, std::min(lo.MinRate, lo.MaxRate));
    const float maxRate = std::max(minRate, std::max(lo.MinRate, lo.MaxRate));
    if (lo.MatchSpeed && speed > 0.05f) {
        if (walkRef > 0.0f) pose.WalkRate = std::clamp(speed / walkRef, minRate, maxRate);
        if (sprintRef > 0.0f) pose.SprintRate = std::clamp(speed / sprintRef, minRate, maxRate);
    }

    m_Pose = pose;
    return m_Pose;
}
