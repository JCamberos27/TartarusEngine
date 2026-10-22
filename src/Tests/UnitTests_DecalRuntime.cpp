#include "UnitTestSupport.h"
#include "../Game/BulletHoles.h"
#include "../Game/Components.h"
#include "../Game/SprayCan.h"
#include "../Game/World.h"
#include "../Graffiti/GraffitiTimeline.h"
#include "../Renderer/BloodRenderer.h"
#include "../Renderer/DecalSystem.h"
#include "../Renderer/GeneratedDecalMaps.h"

#include <glm/gtc/packing.hpp>
#include <cmath>
#include <vector>

// Decals P4's runtime decals (docs/DECALS.md, "Runtime decals"): blood stains and bullet holes on the
// clustered decals, the maps the engine makes for them, and the gameplay spray can's pose and reveal.
// Pure CPU. Run with --unit-tests "Decal runtime".
namespace {

bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) { return glm::length(a - b) < eps; }

void BloodSetMaps() {
    // Two texels: a covered core (norm alpha 0.6 -> coverage 1, reveal 1, thick) and bare (no coverage).
    const uint8_t norm[8] = {200, 60, 0, 153, 128, 128, 0, 0};
    const uint8_t mask[8] = {255, 0, 255, 255, 0, 0, 0, 255};
    const GeneratedDecalMaps::Images im = DecalBake::BloodSet(norm, mask, 2, 1);
    CHECK(im.Width == 2 && im.Height == 1 && im.Albedo.size() == 8 && im.Aux.size() == 2 && im.Normal.size() == 8);
    CHECK(im.Albedo[3] == 153 && im.Albedo[7] == 0); // the raw alpha (its x2 gain comes after filtering)
    CHECK(im.Aux[0] == 255 && im.Aux[1] == 0);       // the reveal order is the mask's red
    CHECK(im.Albedo[0] < im.Albedo[4]);              // the pooled core is shaded darker
    CHECK(im.Normal[0] == 200 && im.Normal[1] == 60); // the slope carried over
}

void HoleMaps() {
    // One 8x8 cell, fully opaque: aux falls from ~1 at the centre to 0.5 at the edge's middle (HoleAux).
    std::vector<uint8_t> rgba(8 * 8 * 4, 255);
    const GeneratedDecalMaps::Images im = DecalBake::KnifeLayer(rgba.data(), nullptr, 8, 1, 1, DecalBake::KnifeMode::Hole);
    CHECK(im.Normal.empty() && im.Aux.size() == 64);
    const int centre = im.Aux[3 * 8 + 3], edge = im.Aux[3 * 8 + 0];
    CHECK(centre > 225 && edge < centre && edge > 120);
    // The rim cut: texels past `rim` cell half-widths fall under the threshold, those well inside it over.
    const float rim = 0.8f;
    CHECK(DecalBake::HoleAux(0.3f) >= DecalBake::HoleRimThreshold(rim) + DecalBake::HoleRimBand(rim));
    CHECK(DecalBake::HoleAux(0.95f) < DecalBake::HoleRimThreshold(rim));

    const GeneratedDecalMaps::Images a = DecalBake::ProceduralHole(16), b = DecalBake::ProceduralHole(16);
    CHECK(a.Width == 32 && a.Height == 32 && a.Albedo == b.Albedo && a.Normal == b.Normal); // deterministic
    const size_t mid = (size_t)(8 * 32 + 8) * 4, corner = 0;
    CHECK(a.Albedo[mid + 3] > 240 && a.Albedo[mid] < 20); // the bore: opaque and black
    CHECK(a.Albedo[corner + 3] < 40);                      // the cell's corner: next to nothing
}

void PackRuntimeDecal() {
    DecalTextureArrays::Slot slot;
    slot.Alloc.Layer = 3;
    slot.Alloc.Size = 512;
    slot.Rect = glm::vec4(0.25f, 0.5f, 0.125f, 0.125f);
    slot.MaxLod = 7;
    slot.TexFlags = Decals::FLAG_HAS_NORMAL_TEX | Decals::FLAG_HAS_AUX_TEX;
    RuntimeDecal r;
    r.Cols = r.Rows = 2;
    r.Frame = 3;
    r.NormalOp = 0.5f;
    r.RoughnessOp = 1.0f;
    r.Roughness = 0.2f;
    r.Wear = 0.25f;
    r.Reveal = true;
    r.RevealThreshold = 0.4f;
    r.RevealBand = 0.1f;
    r.AffectDynamic = false;
    r.Liquid = true;
    Decals::GpuDecal g{};
    DecalSystem::PackRuntime(r, slot, g);
    CHECK((g.Ids.z & Decals::FLAG_LIQUID) != 0u);
    CHECK(Near(glm::vec3(g.Rect), glm::vec3(0.3125f, 0.5625f, 0.0625f))); // the bottom-right cell
    CHECK((g.Ids.x & 0xFFu) == 3u);
    const uint32_t ch = g.Ids.x >> 16 & 0xFFu, flags = g.Ids.z;
    CHECK((ch & Decals::CH_ALBEDO) && (ch & Decals::CH_NORMAL) && (ch & Decals::CH_ROUGHNESS) && !(ch & Decals::CH_EMISSIVE));
    CHECK((flags & Decals::FLAG_REVEAL) && (flags & Decals::FLAG_STATIC_RECEIVERS) && !(flags & Decals::FLAG_DYNAMIC_RECEIVERS));
    const glm::vec2 tb = glm::unpackHalf2x16(g.H2.w), rw = glm::unpackHalf2x16(g.H2.y), rm = glm::unpackHalf2x16(g.H2.x);
    CHECK(std::abs(tb.x - 0.4f) < 1e-3f && std::abs(tb.y - 0.1f) < 1e-3f);
    CHECK(std::abs(rw.y - 0.25f) < 1e-3f && std::abs(rm.x - 0.2f) < 1e-3f); // dry wear, wet roughness
    // No aux map decoded: no reveal (it would hide everything), and no normal channel without a normal map.
    slot.TexFlags = 0;
    DecalSystem::PackRuntime(r, slot, g);
    CHECK(!(g.Ids.z & Decals::FLAG_REVEAL) && !((g.Ids.x >> 16) & Decals::CH_NORMAL));
    // Bits never collide with the substrate flag merged in from P3.
    CHECK(Decals::FLAG_REVEAL != Decals::FLAG_SUBSTRATE_AUTO && Decals::FLAG_LIQUID != Decals::FLAG_REVEAL &&
          Decals::FLAG_LIQUID < (1u << Decals::HEIGHT_BLEND_SHIFT));
}

void BoxesFromBloodAndHoles() {
    // The blood box (+Y out of the surface, image top at -Z) as a decal box (+Z out, image top at +Y).
    const glm::mat4 model(glm::vec4(0.4f, 0, 0, 0), glm::vec4(0, 0.1f, 0, 0), glm::vec4(0, 0, 0.3f, 0), glm::vec4(1, 2, 3, 1));
    const glm::mat4 box = BloodRenderer::DecalBox(model, false);
    CHECK(Near(glm::vec3(box[2]), glm::vec3(0, 0.1f, 0)) && Near(glm::vec3(box[1]), glm::vec3(0, 0, -0.3f)));
    CHECK(Near(glm::vec3(box[0]), glm::vec3(0.4f, 0, 0)) && Near(glm::vec3(box[3]), glm::vec3(1, 2, 3)));
    CHECK(Near(glm::vec3(BloodRenderer::DecalBox(model, true)[0]), glm::vec3(-0.4f, 0, 0)));

    // A hole in a crate is a decal in the crate's space: it follows the crate, +Z out of the face struck.
    World world;
    BulletHoleList holes;
    const entt::entity crate = world.Registry.create();
    auto& t = world.Registry.emplace<TransformComponent>(crate);
    t.Position = glm::vec3(10, 0, 0);
    world.RebuildWorldTransformCache();
    holes.Add(world, crate, glm::vec3(10.5f, 0, 0), glm::vec3(1, 0, 0), 0.004f, 7, 0.12f, 0.8f);
    holes.Add(world, entt::null, glm::vec3(0, 1, 0), glm::vec3(0, 1, 0), 0.004f);
    t.Position = glm::vec3(0, 0, -5);
    t.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    world.RebuildWorldTransformCache();
    std::vector<RuntimeDecal> out;
    const int n = holes.Emit(world, out, [](int, RuntimeDecal&) { return true; });
    CHECK(n == 2 && out.size() == 2);
    if (out.size() == 2) {
        const RuntimeDecal& k = out[0];
        CHECK(Near(glm::vec3(k.BoxToWorld[3]), glm::vec3(0, 0, -5.5f)));
        CHECK(Near(glm::normalize(glm::vec3(k.BoxToWorld[2])), glm::vec3(0, 0, -1)));
        CHECK(std::abs(glm::length(glm::vec3(k.BoxToWorld[0])) - 0.12f) < 1e-4f &&
              std::abs(glm::length(glm::vec3(k.BoxToWorld[2])) - BulletHoleList::kKnifeDepth) < 1e-4f);
        CHECK(k.Reveal && std::abs(k.RevealThreshold - DecalBake::HoleRimThreshold(0.8f)) < 1e-6f);
        const RuntimeDecal& p = out[1]; // the procedural one: 5 hole radii across, no rim cut
        CHECK(std::abs(glm::length(glm::vec3(p.BoxToWorld[0])) - 0.004f * DecalBake::kProceduralHoleSpan) < 1e-5f && !p.Reveal);
        CHECK(std::abs(glm::determinant(glm::mat3(p.BoxToWorld))) > 0.0f);
    }
    // A hole whose maps aren't made yet is skipped this frame.
    out.clear();
    CHECK(holes.Emit(world, out, [](int d, RuntimeDecal&) { return d < 0; }) == 1);
}

void SprayCanPose() {
    // On a wall: +Z out of it, the image's top up the wall, right-handed.
    const glm::mat3 m = glm::mat3_cast(SprayCanSystem::WallRotation(glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)));
    CHECK(Near(m[2], glm::vec3(0, 0, 1)) && Near(m[1], glm::vec3(0, 1, 0)) && Near(m[0], glm::vec3(1, 0, 0)));
    // On a floor: the top away from the can.
    const glm::mat3 f = glm::mat3_cast(SprayCanSystem::WallRotation(glm::vec3(0, 1, 0), glm::vec3(1, -1, 0)));
    CHECK(Near(f[2], glm::vec3(0, 1, 0)) && Near(f[1], glm::vec3(1, 0, 0)));

    // An NPC's can: from its hand along the way it faces (+Z of its transform), named after it.
    World world;
    const entt::entity npc = world.CreateEmptyEntity(glm::vec3(2, 0, 1), glm::vec3(0, 180, 0), glm::vec3(1), "Tenant");
    const SprayCanAction a = SprayCanAction::ForCharacter(world, npc, "assets/x.graffiti", 1.3f);
    CHECK(Near(a.From, glm::vec3(2, 1.3f, 1)) && Near(a.Direction, glm::vec3(0, 0, -1)) && a.Painter == "Tenant");

    // The piece goes on over its duration: nothing at the start, all of it at the end, never back.
    CHECK(Graffiti::RevealThreshold(0.0f, 0.04f) >= 1.0f && Graffiti::RevealThreshold(1.0f, 0.04f) == 0.0f);
    float last = 2.0f;
    bool falling = true;
    for (int i = 0; i <= 20; ++i) {
        const float t = Graffiti::RevealThreshold(i / 20.0f, 0.04f);
        falling &= t <= last;
        last = t;
    }
    CHECK(falling);
    SprayPaintComponent sp;
    sp.Seconds = 8.0f;
    sp.Elapsed = 2.0f;
    CHECK(std::abs(SprayCanSystem::Progress(sp) - 0.25f) < 1e-6f);
    sp.Elapsed = 20.0f;
    CHECK(SprayCanSystem::Progress(sp) == 1.0f);
}

} // namespace

void RegisterDecalRuntimeTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Decal runtime: blood set maps (coverage, shade, reveal order)", BloodSetMaps});
    tests.push_back({"Decal runtime: bullet hole maps and the rim cut", HoleMaps});
    tests.push_back({"Decal runtime: a runtime decal packed for the GPU", PackRuntimeDecal});
    tests.push_back({"Decal runtime: blood and bullet hole boxes (entity space)", BoxesFromBloodAndHoles});
    tests.push_back({"Decal runtime: spray can pose, NPC can and reveal", SprayCanPose});
}
