#include "FirstPersonProcedural.h"
#include "SwayModifierSerialization.h"

#include "AnimatorController.h"
#include "Components.h"
#include "AtomicFile.h"
#include <filesystem>
#include <fstream>

#include <glm/gtc/constants.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

WeaponRecoilSettings RecoilAsset::Defaults() {
    return WeaponRecoilSettings{}; // a new profile starts empty (RecoilAnimData's defaults) and is authored in the editor
}

bool RecoilAsset::FromJsonString(const std::string& text, RecoilAsset& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("type", json()) != "recoil" ||
        (j.value("version", json()) != 1 && j.value("version", json()) != 2) ||
        !j.contains("recoil") || !j["recoil"].is_object()) {
        if (error) *error = "Expected a version 2 RecoilAnimData asset (version 1 migrates on save)";
        return false;
    }
    RecoilAsset parsed;
    parsed.Data = Defaults();
    if (j["version"] == 1) {
        // The old tuning over the old profile defaults, converted.
        LegacyRecoilSettings legacy = LegacyRecoilSettings::Defaults();
        legacy.AimPitch = {0.35f, 0.55f};
        legacy.AimYaw = {-0.08f, 0.08f};
        legacy.CameraScale = 0.25f;
        if (!LegacyRecoilSettings::FromJson(j["recoil"], legacy, error)) return false;
        parsed.Data.Unity = RecoilAnimData::FromLegacy(legacy);
        if (const auto on = j["recoil"].find("enabled"); on != j["recoil"].end()) {
            if (!on->is_boolean()) { if (error) *error = "'recoil.enabled' must be true or false"; return false; }
            parsed.Data.Enabled = on->get<bool>();
        }
    } else if (!RecoilAnimData::FromJson(j["recoil"],parsed.Data.Unity,error)) return false;
    out = std::move(parsed);
    return true;
}

bool RecoilAsset::LoadFile(const std::string& path, RecoilAsset& out, std::string* error) {
    std::ifstream in(std::filesystem::u8path(path));
    if (!in) { if (error) *error = "Could not open recoil asset '" + path + "'"; return false; }
    return FromJsonString(std::string(std::istreambuf_iterator<char>(in), {}), out, error);
}

std::string RecoilAsset::ToJsonString() const {
    return json{{"type", "recoil"}, {"version", 2}, {"recoil", Data.Unity.ToJson()}}.dump(2) + "\n";
}

bool RecoilAsset::SaveFile(const std::string& path) const {
    RecoilAsset validated;
    const auto text = ToJsonString();
    if (!FromJsonString(text, validated)) return false;
    return AtomicFile::WriteJson(std::filesystem::u8path(path), json::parse(text));
}

namespace {

// Smooth 1D value noise in [-1, 1]: hashed lattice values, quintic-eased between them.
// x is a double so a clock that runs for hours keeps its per-frame steps (a float near 4096 rounds them to 0.0005).
float Noise1(double x, unsigned channel) {
    auto lattice = [channel](long long i) {
        unsigned h = (unsigned)i * 374761393u + channel * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return (float)(h & 0xffffu) / 32767.5f - 1.0f;
    };
    const double f = std::floor(x);
    const float t = (float)(x - f);
    const float e = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    const long long i = (long long)f;
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
        if (it == J.end()) return;
        if (!it->is_boolean()) { Fail(std::string("'") + key + "' must be a boolean"); return; }
        out = it->get<bool>();
    }
    void String(const char* key, std::string& out) {
        const auto it = J.find(key);
        if (it == J.end()) return;
        if (!it->is_string()) { Fail(std::string("'") + key + "' must be a string"); return; }
        out = it->get<std::string>();
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
        if (it == J.end()) return;
        if (!it->is_object()) { Fail(std::string("'") + key + "' must be a spring object"); return; }
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

LegacyRecoilSettings LegacyRecoilSettings::Defaults() {
    LegacyRecoilSettings r;
    r.Rotation.X = FirstPersonKickCurve(1.2f);   // pitch: the old ADS kick's 1.2 degrees
    r.Rotation.Y = FirstPersonKickCurve(0.25f);
    r.Rotation.Z = FirstPersonKickCurve(1.5f);
    r.Position.X = FirstPersonKickCurve(0.0012f);
    r.Position.Y = FirstPersonKickCurve(0.002f);
    r.Position.Z = FirstPersonKickCurve(0.014f); // kickback into the shoulder
    r.CameraPitch = FirstPersonKickCurve(0.45f, 0.08f, 0.16f);
    r.CameraYaw = FirstPersonKickCurve(0.15f, 0.08f, 0.16f);
    r.SustainedRotation = r.Rotation;
    r.SustainedPosition = r.Position;
    return r;
}

WeaponProceduralSettings WeaponProceduralSettings::Defaults() {
    WeaponProceduralSettings s;
    s.Recoil.Unity = RecoilAnimData::FromLegacy(LegacyRecoilSettings::Defaults()); // a new weapon's kick, as it always was

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
        {"enabled", r.Enabled}, {"hipProcedural", r.HipProcedural},
        {"boltCycle", r.BoltCycle}, {"boltBone", r.BoltBone}, {"boltTravel", Vec(r.BoltTravel)},
        {"recoilAnimData", r.Unity.ToJson()},
    };
    j["sway"] = SwayModifierToJson(Sway.Unity);
    j["sway"]["enabled"] = Sway.Enabled;
    const auto& b = Bob;
    j["bob"] = {{"walkStride", b.WalkStride}, {"sprintStride", b.SprintStride}, {"ease", b.Ease}};
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
                {"hipPosition", Vec(Aim.HipPosition)}, {"hipRotation", Vec(Aim.HipRotation)},
                {"blendTime", Aim.BlendTime}, {"blend", Aim.Blend.ToJson()},
                {"positionAdditive", Aim.PositionAdditive}, {"rotationAdditive", Aim.RotationAdditive},
                {"crouchPosition", Vec(Aim.CrouchPosition)}, {"crouchRotation", Vec(Aim.CrouchRotation)},
                {"crouchBlendTime", Aim.CrouchBlendTime}, {"cameraShare", Aim.CameraShare}};
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
               {"offTag", k.OffTag}, {"blendTime", k.BlendTime},
               {"useClipCurves", k.UseClipCurves}, {"curveAll", k.CurveAll}, {"curveRightHand", k.CurveRightHand},
               {"curveLeftHand", k.CurveLeftHand}, {"curveLook", k.CurveLook},
               {"rightHandPosition", Vec(k.RightHandPosition)}, {"rightHandRotation", Vec(k.RightHandRotation)},
               {"leftHandPosition", Vec(k.LeftHandPosition)}, {"leftHandRotation", Vec(k.LeftHandRotation)}};
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
        r.Bool("hipProcedural", o.HipProcedural);
        r.Number("boltCycle", o.BoltCycle);
        r.String("boltBone", o.BoltBone);
        r.Vector("boltTravel", o.BoltTravel);
    });
    // A pre-port sway block (lookRotation, ...) keeps only its "enabled": its other keys are ignored.
    root.Object("sway", [&](Reader& r) { r.Bool("enabled", s.Sway.Enabled); });
    root.Object("bob", [&](Reader& r) {
        auto& o = s.Bob;
        r.Number("walkStride", o.WalkStride); // (a retired gun bob's other keys are ignored)
        r.Number("sprintStride", o.SprintStride);
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
        r.Vector("hipPosition", s.Aim.HipPosition);
        r.Vector("hipRotation", s.Aim.HipRotation);
        r.Number("blendTime", s.Aim.BlendTime);
        r.CurveField("blend", s.Aim.Blend);
        r.Number("positionAdditive", s.Aim.PositionAdditive);
        r.Number("rotationAdditive", s.Aim.RotationAdditive);
        r.Vector("crouchPosition", s.Aim.CrouchPosition);
        r.Vector("crouchRotation", s.Aim.CrouchRotation);
        r.Number("crouchBlendTime", s.Aim.CrouchBlendTime);
        r.Number("cameraShare", s.Aim.CameraShare);
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
        r.Bool("useClipCurves", o.UseClipCurves);
        r.String("curveAll", o.CurveAll);
        r.String("curveRightHand", o.CurveRightHand);
        r.String("curveLeftHand", o.CurveLeftHand);
        r.String("curveLook", o.CurveLook);
        r.Vector("rightHandPosition", o.RightHandPosition);
        r.Vector("rightHandRotation", o.RightHandRotation);
        r.Vector("leftHandPosition", o.LeftHandPosition);
        r.Vector("leftHandRotation", o.LeftHandRotation);
    });
    if (!root.Ok) return false;
    if (const auto it=j.find("sway"); it!=j.end() && it->is_object() && it->contains("aimingSway")) {
        if (!SwayModifierFromJson(*it,s.Sway.Unity,error)) return false;
    }
    if (const auto it = j.find("recoil"); it != j.end() && it->is_object()) {
        if (it->contains("recoilAnimData")) {
            if (!RecoilAnimData::FromJson((*it)["recoilAnimData"],s.Recoil.Unity,error)) return false;
        } else {
            // The pre-RecoilAnimData tuning: converted once, here (saving writes the converted form).
            LegacyRecoilSettings legacy = LegacyRecoilSettings::Defaults();
            if (!LegacyRecoilSettings::FromJson(*it, legacy, error)) return false;
            s.Recoil.Unity = RecoilAnimData::FromLegacy(legacy);
        }
    }
    if (!std::isfinite(s.Recoil.BoltCycle) || s.Recoil.BoltCycle < 0.0f) {
        if (error) *error = "'procedural.recoil.boltCycle' must be finite and nonnegative";
        return false;
    }

    const auto positive = [&](float v, const char* what) {
        if (v > 0.0f) return true;
        if (error) *error = std::string("'procedural.") + what + "' must be positive";
        return false;
    };
    if (!positive(s.Bob.WalkStride, "bob.walkStride") || !positive(s.Bob.SprintStride, "bob.sprintStride") ||
        !positive(s.Breath.Period, "breath.period"))
        return false;
    if (!(s.Locomotion.WalkReference >= 0.0f) || !(s.Locomotion.SprintReference >= 0.0f)) {
        if (error) *error = "'procedural.locomotion' references must be 0 (the player's speed) or positive";
        return false;
    }
    out = std::move(s);
    return true;
}

bool LegacyRecoilSettings::FromJson(const json& recoil, LegacyRecoilSettings& io, std::string* error) {
    if (!recoil.is_object()) { if (error) *error = "'procedural.recoil' must be an object"; return false; }
    LegacyRecoilSettings o = io;
    const json wrapped{{"recoil", recoil}};
    Reader root{wrapped, error};
    root.Object("recoil", [&](Reader& r) {
        r.Vector("adsPivot", o.AdsPivot);
        r.Vector("adsRotationScale", o.AdsRotationScale);
        r.Vector("adsPositionScale", o.AdsPositionScale);
        r.Bool("sustainedCurves", o.SustainedCurves);
        r.Curve3("sustainedRotation", o.SustainedRotation);
        r.Curve3("sustainedPosition", o.SustainedPosition);
        r.Number("sustainedDuration", o.SustainedDuration);
        r.Vector("noiseSide", o.NoiseSide);
        r.Vector("noiseUp", o.NoiseUp);
        r.Number("noiseAcceleration", o.NoiseAcceleration);
        r.Number("noiseDamping", o.NoiseDamping);
        r.Number("noiseAdsScale", o.NoiseAdsScale);
        r.Number("pushAmount", o.PushAmount);
        r.Number("pushAcceleration", o.PushAcceleration);
        r.Number("pushDamping", o.PushDamping);
        r.Vector("swayPitch", o.SwayPitch);
        r.Vector("swayYaw", o.SwayYaw);
        r.Number("swayRoll", o.SwayRoll);
        r.Number("swayAcceleration", o.SwayAcceleration);
        r.Number("swayDamping", o.SwayDamping);
        r.Number("swayAdsScale", o.SwayAdsScale);
        r.Vector("swayPivot", o.SwayPivot);
        r.Number("pitchProgression", o.PitchProgression);
        r.Number("upProgression", o.UpProgression);
        r.Number("progressionAcceleration", o.ProgressionAcceleration);
        r.Number("progressionDamping", o.ProgressionDamping);
        r.Number("progressionAdsScale", o.ProgressionAdsScale);
        r.Vector("aimSmoothing", o.AimSmoothing);
        r.Number("aimAdsScale", o.AimAdsScale);
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
        if (!r.J.contains("adsPivot")) o.AdsPivot = o.Pivot;
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
        r.Number("cameraRoll", o.CameraRoll);
        r.Number("fovPunch", o.FovPunch);
        r.Spring("punchSpring", o.PunchSpring);
    });
    if (!root.Ok) return false;
    const auto positive = [&](float v, const char* what) {
        if (v > 0.0f) return true;
        if (error) *error = std::string("'procedural.recoil.") + what + "' must be positive";
        return false;
    };
    if (!positive(o.Duration, "duration") || !positive(o.Smoothing.Frequency, "smoothing.frequency")) return false;
    const auto& rc = o;
    for (const auto* range : {&rc.PitchRange, &rc.YawRange, &rc.RollRange, &rc.SideRange, &rc.UpRange,
                             &rc.KickRange, &rc.AimPitch, &rc.AimYaw, &rc.CameraYawRange, &rc.NoiseSide,
                             &rc.NoiseUp, &rc.SwayPitch, &rc.SwayYaw}) {
        if (range->x > range->y) { if (error) *error = "Recoil ranges must have min <= max"; return false; }
    }
    for (float value : {rc.AimSmoothing.x, rc.AimSmoothing.y, rc.AimRecoverySpeed, rc.AimRecoveryDelay,
                       rc.NoiseAcceleration, rc.NoiseDamping, rc.PushAcceleration, rc.PushDamping,
                       rc.SwayAcceleration, rc.SwayDamping, rc.ProgressionAcceleration, rc.ProgressionDamping,
                       rc.HipScale, rc.AdsScale, rc.AimAdsScale}) {
        if (!std::isfinite(value) || value < 0.0f) { if (error) *error = "Recoil rates and scales must be finite and nonnegative"; return false; }
    }
    if (!positive(rc.SustainedDuration, "sustainedDuration")) return false;
    if (!(rc.AimRecovery >= 0.0f && rc.AimRecovery <= 1.0f)) { if (error) *error = "'procedural.recoil.aimRecovery' must be 0..1"; return false; }
    io = o;
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

// How much a kick `k` spread over kLandKickSpread must be scaled to swing a spring `sp` - at x0, moving at v0, settling to 0 -
// as far in the kick's direction as the same kick given in one frame. The spring is linear, so its path is the free motion
// plus the kick's own response times the kick: both responses simulated once, then the scale found by bisection (a landing is
// rare, the sums are cheap).
float WeaponProceduralState::LandSpreadGain(const FirstPersonSpring& sp, float dt, float x0, float v0, float k) {
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (std::fabs(k) < 1e-6f) return 1.0f;
    constexpr int kSteps = 90;
    float free[kSteps], once[kSteps], spread[kSteps];
    Spring3 f, a, b;
    f.X.x = x0;
    f.V.x = v0;
    a.V.x = 1.0f;
    float left = kLandKickSpread;
    for (int i = 0; i < kSteps; ++i) {
        if (left > 0.0f) {
            b.V.x += std::min(dt, left) / kLandKickSpread;
            left -= dt;
        }
        f.Step(glm::vec3(0.0f), dt, sp);
        a.Step(glm::vec3(0.0f), dt, sp);
        b.Step(glm::vec3(0.0f), dt, sp);
        free[i] = f.X.x;
        once[i] = a.X.x;
        spread[i] = b.X.x;
    }
    const float dir = k > 0.0f ? 1.0f : -1.0f;
    auto reach = [&](const float* response, float scale) {
        float most = -1e9f;
        for (int i = 0; i < kSteps; ++i) most = std::max(most, dir * (free[i] + scale * k * response[i]));
        return most;
    };
    const float want = reach(once, 1.0f);
    float lo = 1.0f, hi = 3.0f;
    if (reach(spread, lo) >= want) return lo;
    if (reach(spread, hi) <= want) return hi;
    for (int it = 0; it < 24; ++it) {
        const float mid = 0.5f * (lo + hi);
        (reach(spread, mid) < want ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

void WeaponProceduralState::Reset() {
    const auto rng = m_Rng;
    *this = WeaponProceduralState{};
    m_Rng = rng;
}

void WeaponProceduralState::OnShot(const WeaponProceduralSettings& s, bool ads, bool cycleBolt, float rpm, RecoilFireMode mode, double unscaledTime, float frameDt) {
    const auto& r = s.Recoil;
    if (!r.Enabled) return;
    m_Unity.Play(r.Unity,ads,rpm,mode,m_Rng,unscaledTime,frameDt);
    m_SemiRecoveryTime = mode == RecoilFireMode::Semi ? 60.0f / std::max(1.0f,rpm) : -1.0f;
    if (cycleBolt) m_BoltTime = 0;
}

IKCurveWeights SampleIKCurves(const WeaponIKSettings& ik, const AnimatorController& ctrl,
                              const AnimatorControllerComponent& ac, int layer) {
    IKCurveWeights w;
    if (!ik.UseClipCurves) return w;
    const auto get = [&](const std::string& name) {
        return std::clamp(AnimatorSampleCurve(ctrl, ac, layer, name, 1.0f), 0.0f, 1.0f);
    };
    w.All = get(ik.CurveAll);
    w.RightHand = get(ik.CurveRightHand);
    w.LeftHand = get(ik.CurveLeftHand);
    w.Look = get(ik.CurveLook);
    return w;
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
    pose.IKWeight = m_IK * std::clamp(in.IKCurve, 0.0f, 1.0f);

    // Recoil: RecoilAnimation's solver (RecoilAnimation.h).
    const auto& r = s.Recoil;
    if (r.Enabled) {
        m_Unity.Update(r.Unity,in.Ads,dt,m_Rng); // the clamped step: a hitch must not fling the recoil springs
        if (m_SemiRecoveryTime >= 0.0f) {
            m_SemiRecoveryTime = std::max(0.0f,m_SemiRecoveryTime - dt);
            if (m_SemiRecoveryTime <= 0.000001f) {
                SetFiring(false);
                m_SemiRecoveryTime = -1.0f;
            }
        }
        pose.Position = m_Unity.OutLoc * glm::vec3(1,1,-1);
        // Reflect Unity's +Z-forward local rotation into the engine's -Z-forward camera frame.
        glm::quat q(m_Unity.OutRot.w,-m_Unity.OutRot.x,-m_Unity.OutRot.y,m_Unity.OutRot.z);
        float yaw,pitch,roll;
        glm::extractEulerAngleYXZ(glm::mat4_cast(q),yaw,pitch,roll);
        pose.Rotation = glm::degrees(glm::vec3(pitch,yaw,roll));
        pose.AimKick = {m_Unity.RecoilDelta.y,m_Unity.RecoilDelta.x};
        pose.Pivot = glm::vec3(0); // the port already applied hip/aim and sway pivots in sequence
    } else m_Unity = RecoilAnimation{};
    // Bolt: slams back over the first 35% of its cycle (easing out), returns over the rest.
    m_BoltTime += dt;
    if (r.Enabled && r.BoltCycle > 0.0f && m_BoltTime < r.BoltCycle) {
        const float u = m_BoltTime / r.BoltCycle;
        pose.Bolt = u < 0.35f ? std::sin(glm::half_pi<float>() * u / 0.35f)
                              : 0.5f + 0.5f * std::cos(glm::pi<float>() * (u - 0.35f) / 0.65f);
    }

    // (Sway is the SwayModifier port, applied by the presentation on the arms rig before arm IK.)
    // The bob's clock: phase-locked to distance travelled (or the feet), so the camera bob never slides against the
    // footsteps. The gun bob itself is retired (the walk clip's layer moves the gun).
    const auto& b = s.Bob;
    const float speed = glm::length(glm::vec2(in.Velocity.x, in.Velocity.z));
    const float ease = 1.0f - std::exp(-dt * std::max(b.Ease, 0.0f));
    // The camera bob: full from a walk's pace up (Walk Bob at a walk, blending to Sprint Bob at the jog), easing in from a standstill.
    m_CamBobWeight += (std::min(speed / std::max(in.WalkSpeed * 0.9f, 0.5f), 1.0f) - m_CamBobWeight) * ease;
    // The "sprint" bob and stride are the fast gait's: a sprint, or (the player's Shift) the jog, blended in by
    // speed between the walk and the fast gait's speed.
    const float fastGait = in.Sprinting ? 1.0f
        : in.SprintSpeed > in.WalkSpeed + 0.05f ? std::clamp((speed - in.WalkSpeed) / (in.SprintSpeed - in.WalkSpeed), 0.0f, 1.0f) : 0.0f;
    m_BobSprint += (fastGait - m_BobSprint) * ease;
    const float stride = std::max(b.WalkStride + (b.SprintStride - b.WalkStride) * m_BobSprint, 0.01f);
    if (in.HaveGaitPhase && in.GaitPhase >= 0.0f) {
        // On the feet: the bob's footfalls (its phases 0.25 and 0.75, where the dip bottoms out) a beat after the plants, as
        // the weight comes onto the leg. Eased across, so taking the body's clock never jumps the bob.
        float d = in.GaitPhase + 0.2f - m_BobPhase;
        d -= std::floor(d + 0.5f);
        m_BobPhase += d * std::min(1.0f, dt * 10.0f);
        m_BobPhase -= std::floor(m_BobPhase);
    } else {
        m_BobPhase = std::fmod(m_BobPhase + speed * dt / stride, 1.0f);
    }

    // Breathing: a slow idle drift, calmer with sights up.
    const auto& br = s.Breath;
    // Sprinting winds the player; it builds while sprinting and fades once they stop.
    m_Exertion = in.Sprinting ? std::min(1.0f, m_Exertion + (br.ExertionBuild > 0.0f ? dt / br.ExertionBuild : 1.0f))
                              : std::max(0.0f, m_Exertion - (br.ExertionRecover > 0.0f ? dt / br.ExertionRecover : 1.0f));
    const float winded = Smooth01(m_Exertion);
    const float rate = 1.0f + (std::max(br.ExertionRate, 0.0f) - 1.0f) * winded;
    m_BreathPhase = std::fmod(m_BreathPhase + dt * rate / std::max(br.Period, 0.05f), 1.0f);
    m_DriftTime += (double)(dt * rate * std::max(br.DriftFrequency, 0.0f)); // never wrapped: a wrap jumped the noise
    if (br.Enabled) {
        const float scale = adsMix(br.HipScale, br.AdsScale) * (1.0f + (std::max(br.ExertionScale, 0.0f) - 1.0f) * winded);
        pose.Position += br.Position.Evaluate(m_BreathPhase) * scale;
        pose.Rotation.x += br.Pitch.Evaluate(m_BreathPhase) * scale;
        // Two octaves of noise per axis: a slow wander with a little faster texture on it.
        auto drift = [&](unsigned ch) { return 0.75f * Noise1(m_DriftTime, ch) + 0.25f * Noise1(m_DriftTime * 2.7, ch + 17u); };
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
            m_LandPosKick = glm::vec3(0.0f, -hit * jp.LandPosition * kick * jumpScale, 0.0f);
            m_LandRotKick = glm::vec3(-hit * jp.LandPitch * kick * jumpScale, 0.0f, side * hit * jp.LandRoll * kick * jumpScale);
            // Each axis scaled so the dip goes as deep as the one-frame kick's did, from where the air lag left the gun.
            for (int c = 0; c < 3; ++c) {
                m_LandPosKick[c] *= LandSpreadGain(jp.Spring, dt, m_JumpPos.X[c], m_JumpPos.V[c], m_LandPosKick[c]);
                m_LandRotKick[c] *= LandSpreadGain(jp.Spring, dt, m_JumpRot.X[c], m_JumpRot.V[c], m_LandRotKick[c]);
            }
            m_LandLeft = kLandKickSpread;
        }
        // The landing kick goes in over kLandKickSpread, not in one frame: all of it at once jumped the gun ~2 cm and ~3 degrees
        // in the frame of touchdown (and the world body's gun, which carries this pose, with it).
        if (m_LandLeft > 0.0f) {
            const float share = std::min(dt, m_LandLeft) / kLandKickSpread;
            m_JumpPos.V += m_LandPosKick * share;
            m_JumpRot.V += m_LandRotKick * share;
            m_LandLeft -= dt;
        }
        m_JumpPos.Step(airPos * jumpScale, dt, jp.Spring);
        m_JumpRot.Step(airRot * jumpScale, dt, jp.Spring);
        pose.Position += m_JumpPos.X;
        pose.Rotation += m_JumpRot.X;
    }

    // The camera: head bob on the gun bob's stride, a roll into strafes, a dip on landing.
    pose.AdsBlend = ads;
    if (cm.Enabled) {
        const float camScale = adsMix(1.0f, cm.AdsScale);
        const glm::vec2 amp = (cm.WalkBob * (1.0f - m_BobSprint) + cm.SprintBob * m_BobSprint) * m_CamBobWeight * camScale *
                              std::clamp(in.CameraBobScale, 0.0f, 1.0f);
        const float tau = glm::two_pi<float>();
        pose.CameraOffset.y -= amp.x * 0.5f * (1.0f - std::cos(2.0f * tau * m_BobPhase)); // down on each footfall
        pose.CameraRoll += amp.y * std::sin(tau * m_BobPhase);
        if (impact >= s.Jump.MinImpact) {
            const float hit = std::min(impact, std::max(s.Jump.MaxImpact, s.Jump.MinImpact));
            const float kick = glm::two_pi<float>() * std::max(cm.Spring.Frequency, 0.01f) * 1.6f;
            m_CamLandKick = glm::vec3(-hit * cm.LandPitch * kick * camScale, -hit * cm.LandDip * kick * camScale, 0.0f);
            for (int c = 0; c < 2; ++c) m_CamLandKick[c] *= LandSpreadGain(cm.Spring, dt, m_CamMotion.X[c], m_CamMotion.V[c], m_CamLandKick[c]);
            m_CamLandLeft = kLandKickSpread;
        }
        if (m_CamLandLeft > 0.0f) { // (spread as the gun's)
            m_CamMotion.V += m_CamLandKick * (std::min(dt, m_CamLandLeft) / kLandKickSpread);
            m_CamLandLeft -= dt;
        }
        const float strafe = in.Grounded ? in.Velocity.x * cm.StrafeRoll * camScale : 0.0f;
        m_CamMotion.Step(glm::vec3(0.0f, 0.0f, -strafe), dt, cm.Spring);
        pose.CameraOffset.y += m_CamMotion.X.y;
        pose.CameraKick.x += m_CamMotion.X.x;
        pose.CameraRoll += m_CamMotion.X.z;
    }

    // ADS aim offset. (The hip carry, Aim.Hip*, is placed on the whole rig by the presentation:
    // through IK it would fade out with the IK in Draw and Regrip.)
    // Crouched, the sights-up pose gets its own offset on top.
    m_CrouchAds = MoveToward(m_CrouchAds, std::clamp(in.Crouch, 0.0f, 1.0f),
                             s.Aim.CrouchBlendTime > 0.0f ? dt / s.Aim.CrouchBlendTime : 1.0f);
    const glm::vec3 aimPos = s.Aim.Position + s.Aim.CrouchPosition * m_CrouchAds;
    const glm::vec3 aimRot = s.Aim.Rotation + s.Aim.CrouchRotation * m_CrouchAds;
    const float camShare = std::clamp(s.Aim.CameraShare, 0.0f, 1.0f);
    pose.Position = BlendAdsChannel(pose.Position, aimPos * (1.0f - camShare), ads, std::clamp(s.Aim.PositionAdditive, 0.0f, 1.0f));
    pose.Rotation = BlendAdsChannel(pose.Rotation, aimRot, ads, std::clamp(s.Aim.RotationAdditive, 0.0f, 1.0f));
    pose.CameraOffset -= aimPos * (camShare * ads); // the view moves toward the gun by the share the gun doesn't

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
    // Camera positive roll tilts Up toward camera-left; positive lean moves right.
    pose.CameraRoll -= lean * l.Angle;
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
