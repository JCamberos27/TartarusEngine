#include "FirstPersonAnimation.h"
#include "AnimatorController.h"
#include "AtomicFile.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <map>
#include <unordered_set>

using json = nlohmann::json;

namespace {

std::string String(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool Bool(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

float Number(const json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}

// Optional [x, y, z] array. Absent or malformed falls back to `fallback`, so older .fpsanim
// files keep loading; the caller still gets a chance to reject non-finite values.
glm::vec3 Vec3(const json& j, const char* key, const glm::vec3& fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return fallback;
    glm::vec3 out = fallback;
    for (int i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number()) return fallback;
        out[i] = (*it)[i].get<float>();
    }
    return out;
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void RoundFloats(json& j) {
    if (j.is_number_float()) { j = std::round(j.get<double>() * 1e6) / 1e6; return; }
    if (j.is_object() || j.is_array()) for (auto& v : j) RoundFloats(v);
}

} // namespace

const FirstPersonAnimationClip* FirstPersonAnimationSet::Find(const std::string& state) const {
    for (const auto& clip : Clips)
        if (clip.Name == state) return &clip;
    return nullptr;
}

bool FirstPersonAnimationSet::FromJsonString(const std::string& text, FirstPersonAnimationSet& out,
                                              std::string* error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        return Fail(error, std::string("invalid JSON: ") + e.what());
    }
    if (!root.is_object()) return Fail(error, "the root must be a JSON object");

    FirstPersonAnimationSet parsed;
    parsed.ArmsModel = String(root, "armsModel");
    parsed.WeaponModel = String(root, "weaponModel");
    parsed.Controller = String(root, "controller");
    parsed.DefaultState = String(root, "defaultState");
    parsed.ViewRotation = Vec3(root, "viewRotation", glm::vec3(0.0f));
    parsed.WeaponSocket = String(root, "weaponSocket");
    parsed.WeaponRoot = String(root, "weaponRoot");
    parsed.WeaponMountRotation = Vec3(root, "weaponMountRotation", glm::vec3(0.0f));
    if (parsed.ArmsModel.empty()) return Fail(error, "missing required string 'armsModel'");
    if (parsed.WeaponModel.empty()) return Fail(error, "missing required string 'weaponModel'");
    if (!Finite(parsed.ViewRotation))
        return Fail(error, "'viewRotation' must be three finite numbers (Y-X-Z degrees)");
    if (!Finite(parsed.WeaponMountRotation))
        return Fail(error, "'weaponMountRotation' must be three finite numbers (Y-X-Z degrees)");
    if (parsed.WeaponSocket.empty() != parsed.WeaponRoot.empty())
        return Fail(error, "'weaponSocket' and 'weaponRoot' must be given together (or both omitted)");
    for (auto [key, dst] : {std::pair{"armsMaterials", &parsed.ArmsMaterials},
                            std::pair{"weaponMaterials", &parsed.WeaponMaterials}}) {
        const auto it = root.find(key);
        if (it == root.end()) continue;
        if (!it->is_object()) return Fail(error, std::string("'") + key + "' must be an object of material name -> .mat path");
        for (const auto& [name, mat] : it->items()) {
            if (!mat.is_string() || mat.get<std::string>().empty())
                return Fail(error, std::string("'") + key + "." + name + "' must be a .mat path");
            dst->emplace_back(name, mat.get<std::string>());
        }
    }

    if (const auto g = root.find("gameplay"); g != root.end() && g->is_object()) {
        FirstPersonWeaponGameplay& gp = parsed.Gameplay;
        gp.Magazine = std::max(1, (int)Number(*g, "magazine", (float)gp.Magazine));
        gp.RoundsPerMinute = Number(*g, "rpm", gp.RoundsPerMinute);
        gp.AllowFullAuto = Bool(*g, "allowFullAuto", gp.AllowFullAuto);
        gp.ReloadHoldSeconds = Number(*g, "reloadHoldSeconds", gp.ReloadHoldSeconds);
        gp.RegripMin = Number(*g, "regripMin", gp.RegripMin);
        gp.RegripMax = Number(*g, "regripMax", gp.RegripMax);
        // Before the "ads" block, the zoom lived here.
        parsed.Ads.Zoom = Number(*g, "adsZoom", parsed.Ads.Zoom);
        parsed.Ads.ViewModelZoom = Number(*g, "adsViewModelZoom", parsed.Ads.ViewModelZoom);
        parsed.Ads.ZoomTime = Number(*g, "adsZoomTime", parsed.Ads.ZoomTime);
        gp.ImpactImpulse = std::max(0.0f, Number(*g, "impactImpulse", gp.ImpactImpulse));
        gp.ImpactMaxSpeed = std::max(0.0f, Number(*g, "impactMaxSpeed", gp.ImpactMaxSpeed));
        gp.ZeroDistance = std::max(0.0f, Number(*g, "zeroDistance", gp.ZeroDistance));
        gp.BulletHoleRadius = Number(*g, "bulletHoleRadius", gp.BulletHoleRadius);
        gp.BulletHoleRadius = std::clamp(std::isfinite(gp.BulletHoleRadius) ? gp.BulletHoleRadius : 0.0045f, 0.0f, 0.1f);
        if (const auto sl = g->find("sightLine"); sl != g->end() && sl->is_object()) {
            const glm::vec3 o = Vec3(*sl, "origin", glm::vec3(0.0f)), d = Vec3(*sl, "direction", glm::vec3(0.0f));
            if (Finite(o) && Finite(d) && glm::length(d) > 1e-6f) {
                gp.HasSightLine = true;
                gp.SightOrigin = o;
                gp.SightDirection = glm::normalize(d);
            }
        }
        if (!(gp.RoundsPerMinute > 0.0f) || !std::isfinite(gp.RoundsPerMinute))
            return Fail(error, "'gameplay.rpm' must be a positive number");
        if (!(gp.ReloadHoldSeconds > 0.0f)) return Fail(error, "'gameplay.reloadHoldSeconds' must be positive");
        if (gp.RegripMax < gp.RegripMin) std::swap(gp.RegripMin, gp.RegripMax);

        // Before the procedural block existed, ADS recoil was one pitch + offset kick (linear
        // rise, exponential settle) and the ADS bob a sine figure-eight. Carried over as curves
        // of the same shape, so an old weapon keeps its feel until someone retunes it.
        if (!root.contains("procedural")) {
            WeaponProceduralSettings& p = parsed.Procedural;
            if (const auto r = g->find("recoil"); r != g->end() && r->is_object()) {
                const float pitch = Number(*r, "pitch", 1.2f);
                const glm::vec3 offset = Vec3(*r, "offset", glm::vec3(0.0f, 0.002f, 0.014f));
                const float rise = Number(*r, "rise", 0.035f), settle = Number(*r, "settle", 0.08f);
                if (!(rise > 0.0f) || !(settle > 0.0f) || !Finite(offset) || !std::isfinite(pitch))
                    return Fail(error, "'gameplay.recoil' needs positive rise/settle and finite pitch/offset");
                p.Recoil.Duration = rise + 5.0f * settle;
                const float peak = rise / p.Recoil.Duration, tau = settle / p.Recoil.Duration;
                p.Recoil.Rotation = {FirstPersonKickCurve(pitch, peak, tau), {}, {}};
                p.Recoil.Position = {FirstPersonKickCurve(offset.x, peak, tau), FirstPersonKickCurve(offset.y, peak, tau),
                                     FirstPersonKickCurve(offset.z, peak, tau)};
                p.Recoil.PitchRange = p.Recoil.UpRange = p.Recoil.KickRange = p.Recoil.SideRange = glm::vec2(1.0f);
                p.Recoil.HipScale = 0.0f; // it used to kick in ADS only
            }
            if (const auto b = g->find("adsBob"); b != g->end() && b->is_object()) {
                const float stride = Number(*b, "stride", 2.4f), fullSpeed = Number(*b, "fullSpeed", 3.5f);
                if (!(stride > 0.0f) || !(fullSpeed > 0.0f))
                    return Fail(error, "'gameplay.adsBob' stride and fullSpeed must be positive");
                p.Bob.WalkStride = stride;
                p.Bob.WalkFullSpeed = fullSpeed;
                p.Bob.Ease = Number(*b, "ease", p.Bob.Ease);
                p.Bob.Walk = {FirstPersonSineCurve(Number(*b, "side", 0.003f)),
                              FirstPersonSineCurve(Number(*b, "vertical", 0.0015f), 2.0f), {}};
                p.Bob.HipScale = 0.0f;    // ADS only, as before
            }
        }
    }
    if (const auto a = root.find("ads"); a != root.end() && a->is_object()) {
        FirstPersonAdsSettings& ads = parsed.Ads;
        ads.Zoom = Number(*a, "zoom", ads.Zoom);
        ads.ViewModelZoom = Number(*a, "viewModelZoom", ads.ViewModelZoom);
        ads.ZoomTime = Number(*a, "zoomTime", ads.ZoomTime);
        if (a->contains("referenceState")) ads.ReferenceState = String(*a, "referenceState");
        if (const std::string tag = String(*a, "carryTag"); !tag.empty()) ads.CarryTag = tag;
        ads.MatchElbows = Bool(*a, "matchElbows", ads.MatchElbows);
        ads.MatchTwist = Bool(*a, "matchTwist", ads.MatchTwist);
        ads.AimHoldTime = Number(*a, "aimHoldTime", ads.AimHoldTime);
        if (const auto m = a->find("gunMotion"); m != a->end() && m->is_object()) {
            ads.GunMotions.clear();
            for (const auto& [state, v] : m->items()) {
                if (!v.is_object() || state.empty()) continue;
                FirstPersonAdsSettings::GunMotion g{state, Number(v, "rotation", 0.0f), Number(v, "position", 0.0f)};
                g.Rotation = std::clamp(std::isfinite(g.Rotation) ? g.Rotation : 0.0f, 0.0f, 1.0f);
                g.Position = std::clamp(std::isfinite(g.Position) ? g.Position : 0.0f, 0.0f, 1.0f);
                ads.GunMotions.push_back(g);
            }
        }
        ads.SightPivot = Number(*a, "sightPivot", ads.SightPivot);
        if (const auto b = a->find("actionBones"); b != a->end() && b->is_array()) {
            ads.ActionBones.clear();
            for (const auto& bone : *b)
                if (bone.is_string() && !bone.get<std::string>().empty()) ads.ActionBones.push_back(bone.get<std::string>());
        }
    }
    {
        FirstPersonAdsSettings& ads = parsed.Ads;
        ads.Zoom = std::clamp(std::isfinite(ads.Zoom) ? ads.Zoom : 1.0f, 1.0f, 8.0f);
        ads.ViewModelZoom = std::clamp(std::isfinite(ads.ViewModelZoom) ? ads.ViewModelZoom : 1.0f, 1.0f, 4.0f);
        ads.ZoomTime = std::clamp(std::isfinite(ads.ZoomTime) ? ads.ZoomTime : 0.2f, 0.0f, 2.0f);
        ads.AimHoldTime = std::clamp(std::isfinite(ads.AimHoldTime) ? ads.AimHoldTime : 0.15f, 0.0f, 2.0f);
        ads.SightPivot = std::clamp(std::isfinite(ads.SightPivot) ? ads.SightPivot : 0.25f, 0.0f, 2.0f);
    }
    if (const auto m = root.find("muzzle"); m != root.end() && m->is_object()) {
        FirstPersonMuzzleSettings& mz = parsed.Muzzle;
        mz.Auto = Bool(*m, "auto", mz.Auto);
        const glm::vec3 o = Vec3(*m, "origin", mz.Origin), d = Vec3(*m, "direction", mz.Direction);
        if (!Finite(o) || !Finite(d)) return Fail(error, "'muzzle.origin' and 'muzzle.direction' must be three finite numbers");
        mz.Origin = o;
        if (glm::length(d) > 1e-6f) mz.Direction = glm::normalize(d);
        else if (!mz.Auto) return Fail(error, "'muzzle.direction' can't be zero when 'muzzle.auto' is false");
    }
    if (const auto l = root.find("laser"); l != root.end() && l->is_object()) {
        FirstPersonLaserSettings& ls = parsed.Laser;
        ls.Enabled = Bool(*l, "enabled", ls.Enabled);
        const glm::vec3 c = Vec3(*l, "color", ls.Color);
        if (Finite(c)) ls.Color = glm::max(c, glm::vec3(0.0f));
        ls.BeamBrightness = Number(*l, "beamBrightness", ls.BeamBrightness);
        ls.SpotBrightness = Number(*l, "spotBrightness", ls.SpotBrightness);
        ls.BeamBrightness = std::clamp(std::isfinite(ls.BeamBrightness) ? ls.BeamBrightness : 1.1f, 0.0f, 100.0f);
        ls.SpotBrightness = std::clamp(std::isfinite(ls.SpotBrightness) ? ls.SpotBrightness : 9.0f, 0.0f, 100.0f);
    }
    if (const auto p = root.find("procedural"); p != root.end()) {
        std::string why;
        if (!WeaponProceduralSettings::FromJson(*p, parsed.Procedural, &why)) return Fail(error, why);
    }

    // v1: a flat clip list (no controller).
    const auto clipsIt = root.find("clips");
    const bool hasClips = clipsIt != root.end() && clipsIt->is_array() && !clipsIt->empty();
    if (parsed.Controller.empty() && !hasClips)
        return Fail(error, "needs a 'controller' (.controller path), or a v1 non-empty 'clips' array");
    if (hasClips) {
        std::unordered_set<std::string> names;
        for (const json& item : *clipsIt) {
            if (!item.is_object()) return Fail(error, "every item in 'clips' must be an object");
            FirstPersonAnimationClip clip;
            clip.Name = String(item, "name");
            clip.ArmsClip = String(item, "arms");
            clip.ArmsBindPose = Bool(item, "armsBindPose", false);
            clip.WeaponClip = String(item, "weapon");
            clip.Loop = Bool(item, "loop", false);
            clip.Fade = Number(item, "fade", 0.08f);
            if (clip.Name.empty()) return Fail(error, "every clip needs a non-empty 'name'");
            if (clip.ArmsClip.empty() && !clip.ArmsBindPose)
                return Fail(error, "clip '" + clip.Name + "' needs 'arms' or armsBindPose=true");
            if (!std::isfinite(clip.Fade) || clip.Fade < 0.0f || clip.Fade > 5.0f)
                return Fail(error, "clip '" + clip.Name + "' has an invalid 'fade' (expected 0..5)");
            if (!names.insert(clip.Name).second)
                return Fail(error, "duplicate clip name '" + clip.Name + "'");
            parsed.Clips.push_back(std::move(clip));
        }
        if (parsed.DefaultState.empty()) parsed.DefaultState = parsed.Clips.front().Name;
        if (!parsed.Find(parsed.DefaultState))
            return Fail(error, "defaultState '" + parsed.DefaultState + "' does not name a clip");
    }

    out = std::move(parsed);
    return true;
}

bool FirstPersonAnimationSet::LoadFile(const std::string& path, FirstPersonAnimationSet& out,
                                        std::string* error) {
    std::ifstream in(std::filesystem::u8path(path));
    if (!in.is_open()) return Fail(error, "could not open '" + path + "'");
    return FromJsonString(std::string(std::istreambuf_iterator<char>(in), {}), out, error);
}

std::string FirstPersonAnimationSet::ToJsonString() const {
    auto vec3 = [](const glm::vec3& v) { return json::array({v.x, v.y, v.z}); };
    const FirstPersonWeaponGameplay& gp = Gameplay;
    json j;
    j["armsModel"] = ArmsModel;
    j["weaponModel"] = WeaponModel;
    j["controller"] = Controller;
    // A v1 file's clip list and default state: without them an edit saved here would leave a file
    // that no longer loads (no controller, no clips).
    if (!Clips.empty()) {
        json clips = json::array();
        for (const FirstPersonAnimationClip& c : Clips) {
            json item = {{"name", c.Name}, {"arms", c.ArmsClip}};
            if (c.ArmsBindPose) item["armsBindPose"] = true;
            if (!c.WeaponClip.empty()) item["weapon"] = c.WeaponClip;
            if (c.Loop) item["loop"] = true;
            item["fade"] = c.Fade;
            clips.push_back(std::move(item));
        }
        j["clips"] = std::move(clips);
        if (!DefaultState.empty()) j["defaultState"] = DefaultState;
    }
    j["viewRotation"] = vec3(ViewRotation);
    if (!WeaponSocket.empty()) {
        j["weaponSocket"] = WeaponSocket;
        j["weaponRoot"] = WeaponRoot;
        j["weaponMountRotation"] = vec3(WeaponMountRotation);
    }
    for (auto [key, src] : {std::pair{"armsMaterials", &ArmsMaterials}, std::pair{"weaponMaterials", &WeaponMaterials}}) {
        if (src->empty()) continue;
        json m = json::object();
        for (const auto& [name, mat] : *src) m[name] = mat;
        j[key] = std::move(m);
    }
    j["gameplay"] = {
        {"magazine", gp.Magazine},
        {"rpm", gp.RoundsPerMinute},
        {"allowFullAuto", gp.AllowFullAuto},
        {"reloadHoldSeconds", gp.ReloadHoldSeconds},
        {"regripMin", gp.RegripMin},
        {"regripMax", gp.RegripMax},
        {"impactImpulse", gp.ImpactImpulse},
        {"impactMaxSpeed", gp.ImpactMaxSpeed},
        {"zeroDistance", gp.ZeroDistance},
        {"bulletHoleRadius", gp.BulletHoleRadius},
    };
    if (gp.HasSightLine)
        j["gameplay"]["sightLine"] = {{"origin", vec3(gp.SightOrigin)}, {"direction", vec3(gp.SightDirection)}};
    j["ads"] = {
        {"zoom", Ads.Zoom},
        {"viewModelZoom", Ads.ViewModelZoom},
        {"zoomTime", Ads.ZoomTime},
        {"referenceState", Ads.ReferenceState},
        {"carryTag", Ads.CarryTag},
        {"matchElbows", Ads.MatchElbows},
        {"matchTwist", Ads.MatchTwist},
        {"aimHoldTime", Ads.AimHoldTime},
        {"actionBones", Ads.ActionBones},
        {"sightPivot", Ads.SightPivot},
    };
    j["ads"]["gunMotion"] = json::object();
    for (const auto& m : Ads.GunMotions) j["ads"]["gunMotion"][m.State] = {{"rotation", m.Rotation}, {"position", m.Position}};
    j["muzzle"] = {{"auto", Muzzle.Auto}, {"origin", vec3(Muzzle.Origin)}, {"direction", vec3(Muzzle.Direction)}};
    j["laser"] = {{"enabled", Laser.Enabled},
                  {"color", vec3(Laser.Color)},
                  {"beamBrightness", Laser.BeamBrightness},
                  {"spotBrightness", Laser.SpotBrightness}};
    j["procedural"] = Procedural.ToJson();
    RoundFloats(j);
    return j.dump(2);
}

bool FirstPersonAnimationSet::SaveFile(const std::string& path) const {
    return AtomicFile::WriteJson(std::filesystem::u8path(path), json::parse(ToJsonString()));
}

namespace {
std::string ReportKey(const std::string& path) {
    std::string key = std::filesystem::u8path(path).lexically_normal().generic_u8string();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return key;
}
std::map<std::string, FirstPersonBarrelReport>& BarrelReports() {
    static std::map<std::string, FirstPersonBarrelReport> reports;
    return reports;
}
} // namespace

void PublishBarrelReport(const std::string& weaponPath, const FirstPersonBarrelReport& report) {
    BarrelReports()[ReportKey(weaponPath)] = report;
}

const FirstPersonBarrelReport* FindBarrelReport(const std::string& weaponPath) {
    const auto it = BarrelReports().find(ReportKey(weaponPath));
    return it == BarrelReports().end() ? nullptr : &it->second;
}

const char* FirstPersonAnimatorContract::KnownTagDescription(const std::string& tag) {
    for (const auto& t : FirstPersonAnimatorContract::kKnownTags)
        if (tag == t.Name) return t.Description;
    return nullptr;
}

// --- the standard graph ----------------------------------------------------------------------

AnimatorController BuildFirstPersonController(const FirstPersonAnimationSet& set) {
    using AC = AnimatorController;
    namespace K = FirstPersonAnimatorContract;
    AC c;
    c.Tracks = {"arms", "weapon"};
    c.Parameters = {
        {K::kSpeed, AC::ParamType::Float, 0.0f},     {K::kSprint, AC::ParamType::Bool, 0.0f},
        {K::kAim, AC::ParamType::Bool, 0.0f},        {K::kEquipped, AC::ParamType::Bool, 1.0f},
        {K::kAmmo, AC::ParamType::Int, (float)set.Gameplay.Magazine},
        {K::kFire, AC::ParamType::Trigger, 0.0f},    {K::kReload, AC::ParamType::Trigger, 0.0f},
        {K::kMagCheck, AC::ParamType::Trigger, 0.0f},{K::kInspect, AC::ParamType::Trigger, 0.0f},
        {K::kMelee, AC::ParamType::Trigger, 0.0f},   {K::kFidget, AC::ParamType::Trigger, 0.0f},
    };
    AC::Layer& L = c.Layers[0];
    L.Name = "Base Layer";
    L.EntryPosition = {-330.0f, 60.0f};
    L.AnyPosition = {-330.0f, -260.0f};
    L.ExitPosition = {980.0f, -60.0f};

    struct Spec { const char* Name; int Priority; float X, Y; std::vector<std::string> Tags; };
    // Priority = how hard a state is to interrupt from Any State: locomotion 0, transition clips
    // 1, fidgets / fire / inspect 2, reloads and melee 3, equip 4, unarmed 5.
    const std::vector<Spec> specs = {
        {"Idle", 0, 0, 0, {K::kTagIdle}},  {"Walk", 0, 0, 120, {K::kTagReady}},         {"Sprint", 0, 330, 120, {}},
        {"Aim", 0, 0, 240, {K::kTagAds}},  {"IdleToSprint", 1, 330, 0, {}}, {"SprintToIdle", 1, 330, 240, {}},
        {"Regrip", 2, 0, -140, {K::kTagIKOff}}, {"Fire", 2, 660, -330, {K::kTagReady}}, {"Inspect", 2, 660, -255, {K::kTagBusy}},
        {"MagCheck", 2, 660, -180, {K::kTagBusy, K::kTagAdsCarry}}, {"Melee", 3, 660, -105, {K::kTagBusy}},
        {"TacReload", 3, 660, -30, {K::kTagReload, K::kTagAdsCarry}},
        {"EmptyReload", 3, 660, 45, {K::kTagReload, K::kTagAdsCarry}},
        {"Draw", 4, -330, 330, {K::kTagIKOff}}, {"Holster", 4, 330, 400, {K::kTagIKOff}},
        {"Holstered", 5, 0, 440, {K::kTagHidden}},
    };
    auto fadeOf = [&](const char* name) {
        const FirstPersonAnimationClip* clip = set.Find(name);
        return clip ? clip->Fade : 0.08f;
    };
    for (const Spec& sp : specs) {
        // Holstered holds Holster's last frame (hidden anyway) so Draw fades out of the holstered
        // pose rather than through the bind T-pose.
        const FirstPersonAnimationClip* clip = set.Find(std::string(sp.Name) == "Holstered" ? "Holster" : sp.Name);
        if (!clip && std::string(sp.Name) != "Holstered") continue;
        AC::State s;
        s.Name = sp.Name;
        s.Priority = sp.Priority;
        s.Tags = sp.Tags;
        s.Position = {sp.X, sp.Y};
        s.Motions.resize(2);
        if (clip) {
            s.Loop = std::string(sp.Name) == "Holstered" ? false : clip->Loop;
            if (!clip->ArmsBindPose) s.Motions[0].Clip = clip->ArmsClip;
            s.Motions[1].Clip = clip->WeaponClip;
        } else {
            s.Loop = false;
        }
        if (s.Name == "Fire") s.Events = {{K::kEventShot, 0.0f}};
        if (s.Name == "TacReload" || s.Name == "EmptyReload") s.Events = {{K::kEventRefill, 1.0f}};
        L.States.push_back(std::move(s));
    }
    // Real ADS clips: a clip named "ADS_<action>" becomes an "ADS <action>" state tagged ADS (and
    // what the hip action is tagged, minus ADSCarry), which the action's trigger reaches instead
    // of the hip state while Aim is held. Without one, the hip state is carried onto the sights.
    std::vector<std::string> adsVariants;
    for (const char* action : {"TacReload", "EmptyReload", "MagCheck", "Inspect"}) {
        const FirstPersonAnimationClip* clip = set.Find(std::string("ADS_") + action);
        const int hip = L.FindState(action);
        if (!clip || hip < 0) continue;
        AC::State s = L.States[hip];
        s.Name = std::string("ADS ") + action;
        s.Tags.erase(std::remove(s.Tags.begin(), s.Tags.end(), std::string(K::kTagAdsCarry)), s.Tags.end());
        s.Tags.insert(s.Tags.begin(), K::kTagAds);
        s.Position += glm::vec2(260.0f, 0.0f);
        s.Loop = clip->Loop;
        s.Motions.assign(2, {});
        if (!clip->ArmsBindPose) s.Motions[0].Clip = clip->ArmsClip;
        s.Motions[1].Clip = clip->WeaponClip;
        L.States.push_back(std::move(s));
        adsVariants.push_back(action);
    }
    L.DefaultState = L.FindState(set.DefaultState) >= 0 ? set.DefaultState : "Idle";

    auto cond = [](const char* p, AC::Op op, float v = 0.0f) { return AC::Condition{p, op, v}; };
    const AC::Condition moving = cond(K::kSpeed, AC::Op::Greater, 0.05f);
    const AC::Condition still = cond(K::kSpeed, AC::Op::Less, 0.05f);
    const AC::Condition sprint = cond(K::kSprint, AC::Op::If), noSprint = cond(K::kSprint, AC::Op::IfNot);
    const AC::Condition aim = cond(K::kAim, AC::Op::If), noAim = cond(K::kAim, AC::Op::IfNot);
    auto has = [&](const std::string& s) { return L.FindState(s) >= 0; };
    auto add = [&](AC::Source kind, const std::string& from, const std::string& to, std::vector<AC::Condition> cs,
                   float duration, bool exitTime = false) {
        if ((kind == AC::Source::State && !has(from)) || (to != AC::kExitState && !has(to))) return;
        AC::Transition t;
        t.FromKind = kind;
        if (kind == AC::Source::State) t.From = from;
        t.To = to;
        t.Conditions = std::move(cs);
        t.Duration = duration;
        t.HasExitTime = exitTime;
        t.ExitTime = 1.0f;
        if (kind == AC::Source::Any) { t.RespectPriority = true; t.CanTransitionToSelf = true; }
        L.Transitions.push_back(std::move(t));
    };
    using S = AC::Source;
    const char* exitTo = AC::kExitState;

    // Entry: the resting state for the current input (layer start, and every one-shot's return).
    add(S::Entry, "", "Sprint", {sprint, moving}, 0.0f);
    add(S::Entry, "", "Aim", {aim}, 0.0f);
    add(S::Entry, "", "Walk", {moving}, 0.0f);

    // Any State, highest priority first. Holster can't cut Draw short (equal priority) and Draw
    // only leaves Holstered, so 1/2/scroll mid-swap simply queues.
    add(S::Any, "", "Holster", {cond(K::kEquipped, AC::Op::IfNot)}, fadeOf("Holster"));
    // An action's ADS clip goes first: Any State transitions are checked in order, so with Aim
    // held it takes the trigger before the hip state can.
    const auto actionConditions = [&](const std::string& action) -> std::vector<AC::Condition> {
        if (action == "EmptyReload") return {cond(K::kReload, AC::Op::If), cond(K::kAmmo, AC::Op::Less, 0.5f)};
        if (action == "TacReload") return {cond(K::kReload, AC::Op::If), cond(K::kAmmo, AC::Op::Greater, 0.5f)};
        if (action == "MagCheck") return {cond(K::kMagCheck, AC::Op::If)};
        return {cond(K::kInspect, AC::Op::If)};
    };
    for (const std::string& action : adsVariants) {
        std::vector<AC::Condition> cs = actionConditions(action);
        cs.push_back(aim);
        add(S::Any, "", "ADS " + action, std::move(cs), fadeOf(("ADS_" + action).c_str()));
    }
    add(S::Any, "", "EmptyReload", {cond(K::kReload, AC::Op::If), cond(K::kAmmo, AC::Op::Less, 0.5f)}, fadeOf("EmptyReload"));
    add(S::Any, "", "TacReload", {cond(K::kReload, AC::Op::If), cond(K::kAmmo, AC::Op::Greater, 0.5f)}, fadeOf("TacReload"));
    add(S::Any, "", "Melee", {cond(K::kMelee, AC::Op::If)}, fadeOf("Melee"));
    add(S::Any, "", "Fire", {cond(K::kFire, AC::Op::If)}, fadeOf("Fire"));
    add(S::Any, "", "Inspect", {cond(K::kInspect, AC::Op::If)}, fadeOf("Inspect"));
    add(S::Any, "", "MagCheck", {cond(K::kMagCheck, AC::Op::If)}, fadeOf("MagCheck"));
    // Holster's own Any transition must not restart it every frame while unequipped.
    for (auto& t : L.Transitions) if (t.FromKind == S::Any && t.To == "Holster") t.CanTransitionToSelf = false;

    // Locomotion. Sprinting (held AND moving) beats aiming, aiming beats walking; only the
    // Idle<->Sprint pair has transition clips, and aiming out of a sprint skips SprintToIdle.
    add(S::State, "Idle", has("IdleToSprint") ? "IdleToSprint" : "Sprint", {sprint, moving}, fadeOf("IdleToSprint"));
    add(S::State, "Idle", "Aim", {aim}, fadeOf("Aim"));
    add(S::State, "Idle", "Walk", {moving}, fadeOf("Walk"));
    add(S::State, "Idle", "Regrip", {cond(K::kFidget, AC::Op::If)}, fadeOf("Regrip"));
    add(S::State, "Walk", "Sprint", {sprint, moving}, fadeOf("Sprint"));
    add(S::State, "Walk", "Aim", {aim}, fadeOf("Aim"));
    add(S::State, "Walk", "Idle", {still}, fadeOf("Idle"));
    add(S::State, "Aim", "Sprint", {sprint, moving}, fadeOf("Sprint"));
    add(S::State, "Aim", "Walk", {noAim, moving}, fadeOf("Walk"));
    add(S::State, "Aim", "Idle", {noAim, still}, fadeOf("Idle"));
    add(S::State, "Sprint", "Aim", {aim, noSprint}, fadeOf("Aim"));
    add(S::State, "Sprint", "Aim", {aim, still}, fadeOf("Aim"));
    const char* sprintOut = has("SprintToIdle") ? "SprintToIdle" : exitTo;
    add(S::State, "Sprint", sprintOut, {noSprint}, fadeOf("SprintToIdle"));
    add(S::State, "Sprint", sprintOut, {still}, fadeOf("SprintToIdle"));

    // The sprint in / out clips never make the player wait: pressing sprint again partway out
    // goes straight back to sprinting, letting go partway in straight back to rest, and aiming
    // leaves the way out at once. A fidget gives way to moving or aiming the same way.
    add(S::State, "SprintToIdle", "Sprint", {sprint, moving}, 0.1f);
    add(S::State, "SprintToIdle", "Aim", {aim, noSprint}, 0.1f);
    add(S::State, "IdleToSprint", exitTo, {noSprint}, 0.15f);
    add(S::State, "IdleToSprint", exitTo, {still}, 0.15f);
    add(S::State, "Regrip", exitTo, {moving}, 0.15f);
    add(S::State, "Regrip", exitTo, {aim}, 0.15f);

    // One-shots return through Exit when their clip ends; Entry then picks the resting state.
    for (const char* s : {"IdleToSprint", "SprintToIdle", "Regrip", "Fire", "Inspect", "Melee", "Draw"})
        add(S::State, s, exitTo, {}, 0.1f, true);
    // The reloads and the mag check settle back over longer, so a carried ADS action eases its
    // arms into the aim pose instead of snapping.
    for (const char* s : {"MagCheck", "TacReload", "EmptyReload"}) add(S::State, s, exitTo, {}, 0.3f, true);
    for (const std::string& action : adsVariants) add(S::State, "ADS " + action, exitTo, {}, 0.3f, true);
    add(S::State, "Holster", "Holstered", {}, 0.0f, true);
    add(S::State, "Holstered", "Draw", {cond(K::kEquipped, AC::Op::If)}, fadeOf("Draw"));
    return c;
}

float FirstPersonRegripDelay(float unit01, float minSeconds, float maxSeconds) {
    return minSeconds + (maxSeconds - minSeconds) * std::clamp(unit01, 0.0f, 1.0f);
}

FirstPersonReloadInput FirstPersonReloadButton::Update(bool down, float dt) {
    if (down) {
        if (!Down) {
            Down = true;
            Fired = false;
            Held = 0.0f;
            return FirstPersonReloadInput::None;
        }
        Held += dt;
        if (!Fired && Held >= HoldSeconds) {
            Fired = true;
            return FirstPersonReloadInput::MagCheck;
        }
        return FirstPersonReloadInput::None;
    }
    if (!Down) return FirstPersonReloadInput::None;
    Down = false;
    return Fired ? FirstPersonReloadInput::None : FirstPersonReloadInput::Reload;
}

std::vector<FPBody::Check> FirstPersonWeaponValidate(const FirstPersonWeaponCheckInput& in) {
    using namespace FirstPersonAnimatorContract;
    using PT = AnimatorController::ParamType;
    using Sev = FPBody::Severity;
    std::vector<FPBody::Check> out;
    if (!in.Set) return out;
    const FirstPersonAnimationSet& set = *in.Set;
    auto add = [&](Sev level, std::string msg, std::string hint = {}) { out.push_back({level, std::move(msg), std::move(hint)}); };

    if (set.Controller.empty() && set.Clips.empty())
        add(Sev::Error, "No controller.", "Set the Controller (the Animation section): it animates both rigs.");
    if (in.Controller) {
        const AnimatorController& c = *in.Controller;
        struct P { const char* Name; PT Type; Sev Level; const char* What; };
        static const P kParams[] = {
            {kSpeed, PT::Float, Sev::Warning, "The planar speed the walk / sprint blend uses."},
            {kSprint, PT::Bool, Sev::Warning, "True while sprinting: the sprint state."},
            {kAim, PT::Bool, Sev::Warning, "True while aim is held: the ADS state."},
            {kEquipped, PT::Bool, Sev::Warning, "Whether the weapon is wanted in hand: Draw / Holster."},
            {kAmmo, PT::Int, Sev::Info, "Rounds in the magazine (for an empty-reload variant)."},
            {kWalkRate, PT::Float, Sev::Info, "Use it as the walk state's Speed Parameter so the clip follows the player's speed."},
            {kSprintRate, PT::Float, Sev::Info, "Use it as the sprint state's Speed Parameter."},
            {kFire, PT::Trigger, Sev::Warning, "Set on the trigger pull: the fire clip."},
            {kReload, PT::Trigger, Sev::Warning, "Set on R: the reload."},
            {kMagCheck, PT::Trigger, Sev::Info, "Set on a held R: the magazine check."},
            {kInspect, PT::Trigger, Sev::Info, "Set on the Inspect key."},
            {kMelee, PT::Trigger, Sev::Info, "Set on the Melee key."},
            {kFidget, PT::Trigger, Sev::Info, "Set after a long idle: the fidget."},
        };
        for (const P& p : kParams) {
            const AnimatorController::Parameter* found = c.FindParameter(p.Name);
            if (!found)
                add(p.Level, std::string("The controller has no parameter '") + p.Name + "'. " + p.What,
                    p.Level == Sev::Warning ? "Add it in the Animator window's Parameters tab." : "Optional: add it to use this.");
            else if ((p.Type == PT::Trigger) != (found->Type == PT::Trigger))
                add(Sev::Warning, std::string("'") + p.Name + "' should be a " + (p.Type == PT::Trigger ? "Trigger" : "non-trigger parameter") + ".",
                    "Change its type in the Parameters tab.");
        }
        auto anyState = [&](auto pred) {
            for (const auto& L : c.Layers)
                for (const auto& s : L.States)
                    if (pred(s)) return true;
            return false;
        };
        auto hasEvent = [&](const char* name) {
            return anyState([&](const AnimatorController::State& s) {
                for (const auto& e : s.Events)
                    if (e.Name == name) return true;
                return false;
            });
        };
        auto hasTag = [&](const char* tag) { return anyState([&](const AnimatorController::State& s) { return s.HasTag(tag); }); };
        if (!hasEvent(kEventShot))
            add(Sev::Warning, std::string("No state has a '") + kEventShot + "' event.",
                "Add a 'Shot' event on the fire clip at the moment a round leaves the gun; without it hip fire spends no ammo.");
        if (!hasEvent(kEventRefill))
            add(Sev::Warning, std::string("No state has a '") + kEventRefill + "' event.",
                "Add a 'Refill' event on the reload clips at the moment the magazine is in; without it a reload never refills.");
        if (!hasTag(kTagHidden))
            add(Sev::Warning, std::string("No state is tagged '") + kTagHidden + "'.",
                "Tag the unarmed (Holstered) state 'Hidden' so both rigs hide when the weapon is put away.");
        if (!hasTag(kTagAds))
            add(Sev::Warning, std::string("No state is tagged '") + kTagAds + "'.",
                "Tag the aiming state 'ADS' so the zoom, sight alignment and ADS recoil apply.");
        if (!hasTag(kTagReload))
            add(Sev::Info, std::string("No state is tagged '") + kTagReload + "'.", "Tag the reload states so R does nothing and the gun can't fire mid-reload.");
        if (!hasTag(kTagBusy))
            add(Sev::Info, std::string("No state is tagged '") + kTagBusy + "'.", "Tag mag check / inspect / melee 'Busy' so the gun can't fire during them.");
        if (!hasTag(kTagIdle))
            add(Sev::Info, std::string("No state is tagged '") + kTagIdle + "'.", "Tag the settled idle 'Idle' to get the fidget.");
        if (!set.Ads.ReferenceState.empty()) {
            bool found = false;
            for (const auto& L : c.Layers) found = found || L.FindState(set.Ads.ReferenceState) >= 0;
            if (!found)
                add(Sev::Warning, "ADS Reference State '" + set.Ads.ReferenceState + "' is not a state in the controller.",
                    "Pick the aim state in the Aim-Down-Sights section.");
        }
    }
    if (in.HasArmsBone) {
        auto need = [&](const std::string& bone, const char* what) {
            if (bone.empty() || in.HasArmsBone(bone)) return;
            add(Sev::Warning, "The arms rig has no bone '" + bone + "'. " + what,
                "Fix the name in the IK / Rigs & Mount section (bones come from the arms model).");
        };
        const WeaponIKSettings& k = set.Procedural.IK;
        if (k.Enabled) {
            need(k.GunBone, "The gun bone the procedural motion moves.");
            need(k.RightUpper, "Right arm IK.");
            need(k.RightLower, "Right arm IK.");
            need(k.RightHand, "Right arm IK.");
            need(k.LeftUpper, "Left arm IK.");
            need(k.LeftLower, "Left arm IK.");
            need(k.LeftHand, "Left arm IK.");
        }
        need(set.WeaponSocket, "The socket the weapon rides.");
    }
    if (in.HasWeaponBone) {
        if (!set.WeaponRoot.empty() && !in.HasWeaponBone(set.WeaponRoot))
            add(Sev::Warning, "The weapon model has no bone '" + set.WeaponRoot + "' (Weapon Root).", "Set the weapon's root node in Rigs & Mount.");
        const std::string& bolt = set.Procedural.Recoil.BoltBone;
        if (!bolt.empty() && !in.HasWeaponBone(bolt))
            add(Sev::Info, "The weapon model has no bone '" + bolt + "' (Bolt Bone): no procedural bolt.", "Set Bolt Bone in the Recoil section, or clear it.");
    }
    std::stable_sort(out.begin(), out.end(), [](const FPBody::Check& a, const FPBody::Check& b) { return (int)a.Level > (int)b.Level; });
    return out;
}
