#include "UnitTestSupport.h"
#include "../Game/Openable.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Game/SceneSerializer.h"
#include "../Renderer/Model.h"
#include "../Renderer/GIBaker.h"
#include <cstring>
#include "../Core/Paths.h"
#include "../Core/Log.h"
#include "AssetLibrary.h"
#include <filesystem>
#include <json.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {
using Kind = OpenableComponent::MotionKind;

// Where the model-space point `p` lands with the entity at (pos, rot, scale).
glm::vec3 Place(const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scale, const glm::vec3& p) {
    return pos + rot * (scale * p);
}

bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) { return glm::length(a - b) < eps; }

// Each motion from a 50 x 70 x 2 cm door panel: the hinge line stays put and the free edge comes out the front.
void OpenablePoses() {
    const glm::vec3 lo(-0.25f, 0.0f, -0.01f), hi(0.25f, 0.7f, 0.01f), one(1.0f);
    const glm::vec3 base(1.0f, 0.5f, 2.0f);
    const glm::quat id(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 pos;
    glm::quat rot;
    auto pose = [&](Kind k, float amount, float t, const glm::quat& baseRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                    const glm::vec3& scale = glm::vec3(1.0f)) {
        Openable::Pose((int)k, amount, lo, hi, scale, base, baseRot, t, pos, rot);
    };

    pose(Kind::HingeLeft, 90.0f, 0.0f); // shut: the authored pose
    CHECK(Near(pos, base) && Near(glm::vec3(glm::eulerAngles(rot)), glm::vec3(0.0f)));

    pose(Kind::HingeLeft, 90.0f, 1.0f);
    CHECK(Near(Place(pos, rot, one, {lo.x, 0.35f, hi.z}), Place(base, id, one, {lo.x, 0.35f, hi.z})));
    const glm::vec3 freeL = Place(pos, rot, one, {hi.x, 0.35f, hi.z});
    CHECK(freeL.z > base.z + 0.45f && std::abs(freeL.x - (base.x + lo.x)) < 1e-3f); // straight out the front

    // Swing Back: the mirror image, the pin on the back face and the free edge out the back.
    Openable::Pose((int)Kind::HingeLeft, 90.0f, lo, hi, one, base, id, 1.0f, pos, rot, /*back=*/true);
    CHECK(Near(Place(pos, rot, one, {lo.x, 0.35f, lo.z}), Place(base, id, one, {lo.x, 0.35f, lo.z})));
    CHECK(Place(pos, rot, one, {hi.x, 0.35f, lo.z}).z < base.z - 0.45f);

    pose(Kind::HingeRight, 90.0f, 1.0f);
    CHECK(Near(Place(pos, rot, one, {hi.x, 0.35f, hi.z}), Place(base, id, one, {hi.x, 0.35f, hi.z})));
    CHECK(Place(pos, rot, one, {lo.x, 0.35f, hi.z}).z > base.z + 0.45f);

    pose(Kind::Lid, 90.0f, 1.0f); // hinged at the back, its front edge goes up
    CHECK(Near(Place(pos, rot, one, {0.0f, lo.y, lo.z}), Place(base, id, one, {0.0f, lo.y, lo.z})));
    CHECK(Place(pos, rot, one, {0.0f, lo.y, hi.z}).y > base.y + 0.015f);

    pose(Kind::FlapDown, 90.0f, 1.0f); // hinged at the bottom, its top comes forward and down
    const glm::vec3 top = Place(pos, rot, one, {0.0f, hi.y, hi.z});
    CHECK(top.z > base.z + 0.65f && top.y < base.y + 0.05f);

    pose(Kind::FlapUp, 90.0f, 1.0f); // hinged at the top, its bottom comes forward and up
    const glm::vec3 bottom = Place(pos, rot, one, {0.0f, lo.y, hi.z});
    CHECK(bottom.z > base.z + 0.65f && bottom.y > base.y + 0.65f);

    pose(Kind::SlideOut, 0.4f, 1.0f);
    CHECK(Near(pos, base + glm::vec3(0.0f, 0.0f, 0.4f)) && Near(glm::vec3(glm::eulerAngles(rot)), glm::vec3(0.0f)));
    pose(Kind::SlideLeft, 0.4f, 1.0f);
    CHECK(Near(pos, base + glm::vec3(-0.4f, 0.0f, 0.0f)));
    pose(Kind::SlideRight, 0.4f, 0.5f); // eased: halfway in time is halfway there
    CHECK(Near(pos, base + glm::vec3(0.2f, 0.0f, 0.0f)));

    // Turned to face +X, the door opens toward +X; scaled up, the hinge line still stays put.
    const glm::quat yaw = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    pose(Kind::HingeLeft, 90.0f, 1.0f, yaw, glm::vec3(2.0f));
    CHECK(Near(Place(pos, rot, glm::vec3(2.0f), {lo.x, 0.35f, hi.z}), Place(base, yaw, glm::vec3(2.0f), {lo.x, 0.35f, hi.z})));
    CHECK(Place(pos, rot, glm::vec3(2.0f), {hi.x, 0.35f, hi.z}).x > base.x + 0.9f);

    // The usual amounts.
    CHECK(Openable::DefaultAmount((int)Kind::HingeLeft, lo, hi, 0.0f) == 100.0f);
    CHECK(std::abs(Openable::DefaultAmount((int)Kind::SlideOut, lo, hi, 0.6f) - 0.42f) < 1e-5f);
    CHECK(Openable::DefaultAmount((int)Kind::SlideOut, lo, hi, 0.0f) == 0.3f);
    CHECK(std::abs(Openable::DefaultAmount((int)Kind::SlideRight, lo, hi, 0.0f) - 0.45f) < 1e-5f);
}

void OpenableRayBox() {
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -2.0f));
    const glm::vec3 lo(-0.5f), hi(0.5f);
    CHECK(std::abs(Openable::RayBox(at, lo, hi, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f)) - 1.5f) < 1e-4f);
    CHECK(Openable::RayBox(at, lo, hi, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f)) < 0.0f);  // behind
    CHECK(Openable::RayBox(at, lo, hi, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)) < 0.0f); // beside
    CHECK(Openable::RayBox(at, lo, hi, glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == 0.0f); // inside
    // A box turned 45 degrees is hit on its corner.
    const glm::mat4 turned = glm::rotate(at, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(std::abs(Openable::RayBox(turned, lo, hi, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f)) - (2.0f - std::sqrt(0.5f))) < 1e-4f);
}

// A scene's Openable block loads and saves, and in a world Interact toggles the part under the crosshair.
void OpenableInWorld() {
    using Json = nlohmann::json;
    {
        World world;
        AssetLibrary assets;
        const Json node = {{"id", 0}, {"name", "Door"}, {"Openable", {{"Motion", "Slide Out"}, {"Amount", 0.25}, {"Start Open", true}, {"Start Ajar", 0.4}}}};
        CHECK(SceneSerializer::LoadFromString(world, assets, Json{{"formatVersion", 4}, {"empties", Json::array({node})}}.dump()));
        auto v = world.Registry.view<OpenableComponent>();
        CHECK(v.size() == 1);
        if (v.size() == 1) {
            const auto& o = v.get<OpenableComponent>(v.front());
            CHECK(o.Motion == (int)Kind::SlideOut && o.Amount == 0.25f && o.StartOpen && o.StartAjar == 0.4f && o.OpenTime == 0.5f);
        }
        const std::string saved = SceneSerializer::SaveToString(world);
        CHECK(saved.find("\"Openable\"") != std::string::npos && saved.find("Slide Out") != std::string::npos);
    }

    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        Log::Info("OpenableInWorld: no test model - skipped.");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    const glm::vec3 doorAt(0.0f, 0.0f, -1.5f);
    const entt::entity door = w.CreateModelEntity(model, doorAt, glm::vec3(0.0f), glm::vec3(1.0f), "Door");
    auto& o = w.Registry.emplace<OpenableComponent>(door);
    o.Motion = (int)Kind::SlideRight;
    o.Amount = 0.5f;
    o.OpenTime = 0.2f;
    w.RebuildWorldTransformCache();
    const glm::vec3 eye(0.0f), ahead(0.0f, 0.0f, -1.0f);
    const glm::vec3 lo = model->BoundsMin(), hi = model->BoundsMax();
    const glm::vec3 aim = glm::normalize(doorAt + (lo + hi) * 0.5f - eye);

    Openable::Update(w, eye, aim, false, 0.1f); // nothing pressed: stays shut
    CHECK(Near(w.Registry.get<TransformComponent>(door).Position, doorAt));
    Openable::Update(w, eye, aim, true, 0.1f); // pressed: opening, halfway in time
    CHECK(o.Target == 1.0f && std::abs(o.Progress - 0.5f) < 1e-4f);
    Openable::Update(w, eye, aim, false, 0.5f);
    CHECK(o.Progress == 1.0f && Near(w.Registry.get<TransformComponent>(door).Position, doorAt + glm::vec3(0.5f, 0.0f, 0.0f)));
    w.RebuildWorldTransformCache();
    const glm::vec3 aimOpen = glm::normalize(doorAt + glm::vec3(0.5f, 0.0f, 0.0f) + (lo + hi) * 0.5f - eye);
    Openable::Update(w, eye, aimOpen, true, 1.0f); // pressed again, on it where it is now: shut
    CHECK(o.Target == 0.0f && Near(w.Registry.get<TransformComponent>(door).Position, doorAt));
    Openable::Update(w, eye, -ahead, true, 1.0f); // looking away: nothing happens
    CHECK(o.Target == 0.0f);
    Openable::Update(w, glm::vec3(0.0f, 0.0f, 3.0f), ahead, true, 1.0f); // out of reach
    CHECK(o.Target == 0.0f);

    Openable::SetAll(w, 1.0f);
    CHECK(Near(w.Registry.get<TransformComponent>(door).Position, doorAt + glm::vec3(0.5f, 0.0f, 0.0f)));
    Openable::SetAll(w, 0.0f);
    CHECK(Near(w.Registry.get<TransformComponent>(door).Position, doorAt));

    // Start Open is posed open as Play starts; Start Ajar starts it part of the way, and one Interact opens it all the way.
    o.StartOpen = true;
    o.StartAjar = 0.3f;
    o.Started = false;
    Openable::Update(w, eye, -ahead, false, 0.0f);
    CHECK(o.Target > 0.0f && o.Target < 0.5f && Near(w.Registry.get<TransformComponent>(door).Position, doorAt + glm::vec3(0.15f, 0.0f, 0.0f)));
    w.RebuildWorldTransformCache();
    const glm::vec3 aimAjar = glm::normalize(doorAt + glm::vec3(0.15f, 0.0f, 0.0f) + (lo + hi) * 0.5f - eye);
    Openable::Update(w, eye, aimAjar, true, 1.0f);
    CHECK(o.Target == 1.0f && Near(w.Registry.get<TransformComponent>(door).Position, doorAt + glm::vec3(0.5f, 0.0f, 0.0f)));
}

// --bake-gi's door pose (GI Phase 2 Part A): PoseStart opens only the Start Open doors, Restore puts back every
// transform bit for bit and leaves them unstarted, as the scene was loaded.
void OpenablePoseStartRestore() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        Log::Info("OpenablePoseStartRestore: no test model - skipped.");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    const glm::vec3 openAt(1.0f, 0.0f, -1.5f), shutAt(-1.0f, 0.0f, -1.5f);
    const entt::entity a = w.CreateModelEntity(model, openAt, glm::vec3(0.0f), glm::vec3(1.0f), "DoorStartOpen");
    const entt::entity b = w.CreateModelEntity(model, shutAt, glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(1.0f), "DoorShut");
    auto& oa = w.Registry.emplace<OpenableComponent>(a);
    oa.Motion = (int)Kind::SlideRight;
    oa.Amount = 0.5f;
    oa.StartOpen = true;
    auto& ob = w.Registry.emplace<OpenableComponent>(b);
    ob.Motion = (int)Kind::HingeLeft;
    w.RebuildWorldTransformCache();
    const TransformComponent ta = w.Registry.get<TransformComponent>(a), tb = w.Registry.get<TransformComponent>(b);

    const auto saved = Openable::Save(w);
    CHECK(saved.size() == 2);
    const auto posed = Openable::PoseStart(w);
    CHECK(posed.Open.size() == 1 && posed.Open[0] == "DoorStartOpen" && posed.Shut == 1);
    CHECK(Near(w.Registry.get<TransformComponent>(a).Position, openAt + glm::vec3(0.5f, 0.0f, 0.0f)));
    CHECK(w.Registry.get<TransformComponent>(b).Position == tb.Position && w.Registry.get<TransformComponent>(b).Rotation == tb.Rotation);
    CHECK(oa.Started && ob.Started && oa.Progress == 1.0f && ob.Progress == 0.0f);

    // Posed again after something moved it (an earlier SetAll): back to the Play start pose, not left where it was.
    Openable::SetAll(w, 1.0f);
    CHECK(Openable::PoseStart(w).Open.size() == 1 && ob.Progress == 0.0f);
    CHECK(Near(w.Registry.get<TransformComponent>(b).Position, tb.Position));

    Openable::Restore(w, saved);
    const auto& ra = w.Registry.get<TransformComponent>(a);
    const auto& rb = w.Registry.get<TransformComponent>(b);
    CHECK(std::memcmp(&ra.Position, &ta.Position, sizeof(glm::vec3)) == 0 && std::memcmp(&ra.Rotation, &ta.Rotation, sizeof(glm::quat)) == 0);
    CHECK(std::memcmp(&rb.Position, &tb.Position, sizeof(glm::vec3)) == 0 && std::memcmp(&rb.Rotation, &tb.Rotation, sizeof(glm::quat)) == 0);
    CHECK(!oa.Started && !ob.Started && oa.Progress == 0.0f && oa.Target == 0.0f);
    // Play still starts them as before.
    Openable::Update(w, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), false, 0.0f);
    CHECK(oa.Started && oa.Progress == 1.0f && Near(w.Registry.get<TransformComponent>(a).Position, openAt + glm::vec3(0.5f, 0.0f, 0.0f)));
}

// GIBaker's own door step and its exits (GI Phase 2 Part A; Argus's review): PoseDoors poses and flags the scene,
// and the error path (Abort) and the shutdown / emergency path (RestoreDoors) each put every transform back bit for bit,
// clear Openable::BakePosed, and are no-ops the second time. An inactive Start Open door isn't posed (Play skips it).
void GIBakerDoorsRestoreAfterFailure() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        Log::Info("GIBakerDoorsRestoreAfterFailure: no test model - skipped.");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    const glm::vec3 openAt(1.0f, 0.0f, -1.5f), shutAt(-1.0f, 0.0f, -1.5f), offAt(3.0f, 0.0f, -1.5f);
    const entt::entity a = w.CreateModelEntity(model, openAt, glm::vec3(0.0f), glm::vec3(1.0f), "DoorStartOpen");
    const entt::entity b = w.CreateModelEntity(model, shutAt, glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(1.0f), "DoorShut");
    const entt::entity c = w.CreateModelEntity(model, offAt, glm::vec3(0.0f), glm::vec3(1.0f), "DoorInactive");
    for (entt::entity e : {a, c}) {
        auto& o = w.Registry.emplace<OpenableComponent>(e);
        o.Motion = (int)Kind::SlideRight;
        o.Amount = 0.5f;
        o.StartOpen = true;
    }
    w.Registry.emplace<OpenableComponent>(b).Motion = (int)Kind::HingeLeft;
    w.Registry.emplace<DeactivatedTag>(c); // the authored "active: false"; InactiveTag is derived from it
    w.RebuildWorldTransformCache();
    CHECK(w.Registry.all_of<InactiveTag>(c));
    const TransformComponent t0[3] = {w.Registry.get<TransformComponent>(a), w.Registry.get<TransformComponent>(b),
                                      w.Registry.get<TransformComponent>(c)};
    auto same = [&](entt::entity e, const TransformComponent& t) {
        const auto& r = w.Registry.get<TransformComponent>(e);
        return std::memcmp(&r.Position, &t.Position, sizeof(glm::vec3)) == 0 && std::memcmp(&r.Rotation, &t.Rotation, sizeof(glm::quat)) == 0;
    };
    auto allBack = [&] { return same(a, t0[0]) && same(b, t0[1]) && same(c, t0[2]); };

    // (1) The error path: posed, then Abort (Step's error branch) -> as loaded, failed, flag down.
    {
        GIBaker baker;
        CHECK(!Openable::BakePosed());
        baker.PoseDoors(w, GIBaker::Options::Doors::Start);
        CHECK(baker.DoorsPosed() && Openable::BakePosed());
        CHECK(Near(w.Registry.get<TransformComponent>(a).Position, openAt + glm::vec3(0.5f, 0.0f, 0.0f)));
        CHECK(same(c, t0[2]) && !w.Registry.get<OpenableComponent>(c).Started); // inactive: not posed, not started
        baker.Abort(w, "test failure");
        CHECK(allBack() && !baker.DoorsPosed() && !Openable::BakePosed() && baker.Failed());
        CHECK(!w.Registry.get<OpenableComponent>(a).Started && !w.Registry.get<OpenableComponent>(b).Started);
        baker.RestoreDoors(w); // a second put-back is a no-op
        CHECK(allBack() && !Openable::BakePosed());
    }
    // (2) The shutdown / emergency path: posed all open, then RestoreDoors straight away.
    {
        GIBaker baker;
        baker.PoseDoors(w, GIBaker::Options::Doors::Open);
        CHECK(Openable::BakePosed() && !same(a, t0[0]) && !same(b, t0[1]));
        baker.RestoreDoors(w);
        CHECK(allBack() && !baker.DoorsPosed() && !Openable::BakePosed());
        baker.RestoreDoors(w);
        CHECK(allBack());
    }
    // (3) Posed twice (a re-entry): the second save must not take the posed doors for "as loaded".
    {
        GIBaker baker;
        baker.PoseDoors(w, GIBaker::Options::Doors::Open);
        baker.PoseDoors(w, GIBaker::Options::Doors::Start);
        baker.RestoreDoors(w);
        CHECK(allBack() && !Openable::BakePosed());
    }
    // (4) Shut: nothing moves, but the scene still counts as posed until put back.
    {
        GIBaker baker;
        baker.PoseDoors(w, GIBaker::Options::Doors::Shut);
        CHECK(allBack() && Openable::BakePosed());
        baker.Abort(w, "");
        CHECK(!Openable::BakePosed() && baker.Failed());
    }
}
} // namespace

void RegisterOpenableTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Openable: each motion keeps its hinge and opens out the front", OpenablePoses});
    tests.push_back({"Openable: ray against a placed box", OpenableRayBox});
    tests.push_back({"Openable: scene block, Interact toggles, reach, SetAll, Start Open / Ajar", OpenableInWorld});
    tests.push_back({"Openable: PoseStart opens only Start Open doors, Restore puts them back bit for bit", OpenablePoseStartRestore});
    tests.push_back({"GIBaker: posed doors come back bit for bit after a failure, at shutdown and on re-entry", GIBakerDoorsRestoreAfterFailure});
}
