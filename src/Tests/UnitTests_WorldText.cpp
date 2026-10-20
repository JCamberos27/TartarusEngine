#include "UnitTestSupport.h"
#include "../Renderer/WorldText.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Editor/AssetLibrary.h"
#include "../Game/SceneSerializer.h"
#include "../Core/EnginePaths.h"
#include "../Core/Log.h"
#include <json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using json = nlohmann::json;

bool Near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

// UTF-8 decoding: the measurement signs come through, broken bytes are dropped rather than garbling the rest.
void Utf8() {
    const auto deg = WorldTextLayout::DecodeUtf8("30\xC2\xB0");
    CHECK(deg.size() == 3 && deg[0] == '3' && deg[1] == '0' && deg[2] == 0xB0u);
    const auto dash = WorldTextLayout::DecodeUtf8("1\xE2\x80\x93" "5 m");
    CHECK(dash.size() == 5 && dash[1] == 0x2013u);
    CHECK(WorldTextLayout::DecodeUtf8("ok\xC2").size() == 2);      // cut off at the end
    CHECK(WorldTextLayout::DecodeUtf8("\x80" "a").size() == 1);    // a stray continuation byte
}

void FadeCurve() {
    using WorldTextLayout::Fade;
    CHECK(Fade(10.0f, 25.0f, 35.0f) == 1.0f);
    CHECK(Fade(25.0f, 25.0f, 35.0f) == 1.0f);
    CHECK(Near(Fade(30.0f, 25.0f, 35.0f), 0.5f));
    CHECK(Fade(35.0f, 25.0f, 35.0f) == 0.0f);
    CHECK(Fade(80.0f, 25.0f, 35.0f) == 0.0f);
    CHECK(Fade(28.0f, 25.0f, 35.0f) > Fade(32.0f, 25.0f, 35.0f));
    CHECK(Fade(50.0f, 60.0f, 40.0f) == 0.0f && Fade(30.0f, 60.0f, 40.0f) == 1.0f); // fade past cull: a hard cut
}

// Layout in line heights: alignment on the origin, the block centred on it, unknown signs as '?'.
void LayoutMetrics() {
    std::ifstream in(EnginePaths::Resolve("assets/fonts/JetBrainsMono-Regular.ttf"), std::ios::binary);
    const std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ttf.empty()) {
        Log::Info("WorldText layout: no font - skipped.");
        return;
    }
    const WorldTextLayout::Font font = WorldTextLayout::Bake(ttf);
    CHECK(font.Valid());
    if (!font.Valid()) return;
    for (std::uint32_t cp : WorldTextLayout::Charset()) CHECK(font.Find(cp) != nullptr);
    CHECK(font.Find('0') && font.Find('0')->Visible && !font.Find(' ')->Visible);
    const float adv = font.Find('0')->Advance; // monospaced
    CHECK(adv > 0.3f && adv < 0.9f);

    glm::vec2 ext;
    const auto centre = WorldTextLayout::Layout(font, "0.45 m", 1, &ext);
    CHECK(centre.size() == 5);                       // the space draws nothing
    CHECK(Near(ext.x, 3.0f * adv, 1e-3f));          // half of six advances
    CHECK(Near(ext.y, 0.5f, 1e-4f));                // one line: a line height tall
    float lo = 1e9f, hi = -1e9f, top = -1e9f, bottom = 1e9f;
    for (const auto& q : centre) {
        lo = std::min(lo, q.Lo.x); hi = std::max(hi, q.Hi.x);
        top = std::max(top, q.Hi.y); bottom = std::min(bottom, q.Lo.y);
        CHECK(q.Uv.z > q.Uv.x && q.Uv.w > q.Uv.y);
    }
    CHECK(lo > -ext.x - 0.2f && hi < ext.x + 0.2f && lo < 0.0f && hi > 0.0f);
    CHECK(top < 0.75f && bottom > -0.75f);           // inside the block, give or take the SDF padding

    const auto left = WorldTextLayout::Layout(font, "45", 0, &ext);
    CHECK(Near(ext.x, 2.0f * adv, 1e-3f));
    for (const auto& q : left) CHECK(q.Hi.x > 0.0f);
    const auto right = WorldTextLayout::Layout(font, "45", 2);
    for (const auto& q : right) CHECK(q.Lo.x < 0.0f);

    WorldTextLayout::Layout(font, "A\nB", 1, &ext);
    CHECK(Near(ext.y, 1.05f, 1e-4f));               // two lines at 1.1 spacing
    CHECK(WorldTextLayout::Layout(font, "\xE2\x82\xAC", 1).size() == 1); // euro: not in the atlas, drawn as '?'
    CHECK(WorldTextLayout::Layout(font, "", 1).empty());
}

// The component saves under "World Text" with its enums as labels, and comes back the same.
void RoundTrip() {
    World w;
    AssetLibrary assets;
    const entt::entity e = w.CreateEmptyEntity(glm::vec3(1, 0, 2), glm::vec3(-90, 0, 0), glm::vec3(1), "Label");
    auto& t = w.Registry.emplace<WorldTextComponent>(e);
    t.Text = "30\xC2\xB0\nramp";
    t.Size = 0.4f;
    t.Color = glm::vec3(0.9f, 0.95f, 1.0f);
    t.Brightness = 2.0f;
    t.Face = WorldTextComponent::Upright;
    t.Align = WorldTextComponent::Left;
    t.Outline = 0.15f;
    t.FadeStart = 12.0f;
    t.CullDistance = 20.0f;
    t.OnTop = true;
    const std::string saved = SceneSerializer::SaveToString(w, assets);
    const json j = json::parse(saved);
    CHECK(j.dump().find("\"World Text\"") != std::string::npos);
    CHECK(j.dump().find("\"Upright\"") != std::string::npos);

    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, saved));
    const auto view = back.Registry.view<WorldTextComponent>();
    CHECK(view.size() == 1);
    if (view.empty()) return;
    const auto& r = back.Registry.get<WorldTextComponent>(*view.begin());
    CHECK(r.Text == t.Text);
    CHECK(Near(r.Size, 0.4f) && Near(r.Brightness, 2.0f) && Near(r.Outline, 0.15f));
    CHECK(glm::length(r.Color - t.Color) < 1e-4f);
    CHECK(r.Face == WorldTextComponent::Upright && r.Align == WorldTextComponent::Left);
    CHECK(Near(r.FadeStart, 12.0f) && Near(r.CullDistance, 20.0f) && r.OnTop);
}
} // namespace

void RegisterWorldTextTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"World text: UTF-8 decoding keeps the measurement signs", Utf8});
    tests.push_back({"World text: distance fade and cull", FadeCurve});
    tests.push_back({"World text: SDF atlas and line layout", LayoutMetrics});
    tests.push_back({"World text: component round-trips through the scene file", RoundTrip});
}
