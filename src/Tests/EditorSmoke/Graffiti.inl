// Editor smoke suite: Graffiti. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The Graffiti Designer end to end (decals Phase 3, docs/GRAFFITI_DESIGNER.md), in the Apartment
// Hallway's third-floor corridor: a new .graffiti from the Designer's New button, layers added from its Add menu, a stroke
// sprayed on its canvas, then Spray in Scene view puts the piece on a corridor wall and a
// stroke is sprayed straight onto it. The piece is baked twice more from the Bake button and the
// two bakes must be byte-identical (the four PNGs on disk and their hash); the baked decal must be
// resident and drawn, and a shot of the Scene view shows it. Nothing is saved: the scene is left
// unsaved and the piece's files are deleted at the end.

} // namespace
struct EditorSmokeAccess_Graffiti {
    static GraffitiDesigner* G(EditorLayer& e) { return e.m_Graffiti.get(); }
    static bool& Shown(EditorLayer& e) { return e.m_ShowGraffitiDesigner; }
    static void Open(EditorLayer& e, World& w, AssetLibrary& a, const std::string& path) {
        e.m_Dirty = false; // the scratch scene is never worth keeping: no unsaved-changes prompt
        e.RequestOpenScene(w, a, path);
    }
    static bool Ray(EditorLayer& e, World& w, Camera& cam, glm::vec2 px, glm::vec3& hit) {
        return e.RaycastViewportSurface(w, cam, px, hit);
    }
    static void FocusScene(EditorLayer& e) { e.RequestSceneTabFocus(); }
};
namespace {
namespace smoke_graffiti {
namespace fs = std::filesystem;
using GA = EditorSmokeAccess_Graffiti;
const char* kDesigner = "###GraffitiDesigner";

struct State {
    std::string PiecePath;              // the .graffiti the test made (deleted at the end)
    std::string Bake[4];                // the four PNGs' bytes after the first button bake
    uint64_t Hash = 0;
    int BakesBefore = 0;
};
std::shared_ptr<State> g_State;

std::string ReadAll(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
ImVec2 ViewportCentre(EditorLayer& e) {
    const glm::vec2 p = e.ViewportPos(), s = e.ViewportSize();
    return ImVec2(p.x + s.x * 0.5f, p.y + s.y * 0.5f);
}
Step UntilSecs(std::string name, std::function<bool(Ctx&)> pred, double seconds) {
    auto start = std::make_shared<double>(-1.0);
    return {name, [name, pred, seconds, start](Ctx& c) {
        const double now = ImGui::GetTime();
        if (*start < 0.0) *start = now;
        if (pred(c)) return true;
        if (now - *start > seconds) {
            Fail("step '" + name + "' timed out after " + std::to_string((int)seconds) + " s");
            return true;
        }
        return false;
    }, 100000000};
}
bool BakeIdle(Ctx& c) {
    GraffitiDesigner* g = GA::G(c.E);
    return g && g->BakeStage == 0 && !g->BakeAgain;
}
} // namespace smoke_graffiti

void RegisterGraffitiTests() {
    using namespace smoke_graffiti;
    Add("graffiti: design a piece, spray it onto a hallway wall, bake it twice identically, see the decal", {
        Do("reset the test's state", [](Ctx&) { g_State = std::make_shared<State>(); }),
        FreshScene(), Wait(2),
        Do("Scene tab in front", [](Ctx& c) { GA::FocusScene(c.E); }), Wait(2),
        Do("open the Apartment Hallway", [](Ctx& c) {
            GA::Open(c.E, c.W, c.A, fs::path(ProjectPaths::Resolve("scenes/Apartment Hallway.json")).generic_string());
        }),
        UntilSecs("assets streamed in", [](Ctx& c) { return c.A.AsyncInFlight() == 0; }, 180),
        WaitSeconds(1.5),
        UntilSecs("assets still settled", [](Ctx& c) { return c.A.AsyncInFlight() == 0; }, 60),
        // Walk into the corridor the scene opens on (it opens outside its end) and turn to a side wall,
        // standing across the corridor from it so a 2 m piece fits the view.
        Expect("aim the camera at a corridor wall", [](Ctx& c) {
            const glm::vec3 start = c.Cam.Position;
            const ImVec2 mid = ViewportCentre(c.E);
            float best = -1.0f;
            glm::vec3 bestEye = start;
            float bestYaw = c.Cam.Yaw;
            for (float dx : {3.0f, 4.5f, 6.0f, 7.5f})
                for (float dz = -0.75f; dz <= 0.751f; dz += 0.25f)
                    for (float yaw : {90.0f, -90.0f}) {
                        c.Cam.Position = start + glm::vec3(dx, 0.0f, dz);
                        c.Cam.Yaw = yaw;
                        c.Cam.Pitch = 0.0f;
                        glm::vec3 hit;
                        if (!GA::Ray(c.E, c.W, c.Cam, glm::vec2(mid.x, mid.y), hit)) continue;
                        const float d = glm::length(hit - c.Cam.Position);
                        if (d > best && d < 3.0f) {
                            best = d;
                            bestEye = c.Cam.Position;
                            bestYaw = yaw;
                        }
                    }
            c.Cam.Position = bestEye;
            c.Cam.Yaw = bestYaw;
            c.Cam.Pitch = 0.0f;
            return best > 0.8f ? std::string() : "no corridor wall 0.8-3 m away (best " + std::to_string(best) + ")";
        }),
        WaitSeconds(1.0),
        Do("print the view", [](Ctx& c) {
            std::printf("    [graffiti] camera (%.2f %.2f %.2f) yaw %.0f, viewport %.0fx%.0f\n", c.Cam.Position.x, c.Cam.Position.y,
                        c.Cam.Position.z, c.Cam.Yaw, c.E.ViewportSize().x, c.E.ViewportSize().y);
        }),
        CleanShot("wall before", {"###Scene"}),
        Do("show the Graffiti Designer", [](Ctx& c) {
            GA::Shown(c.E) = true;
            ImGui::SetWindowPos(EditorPanels::GraffitiDesigner, ImVec2(40, 60));
            ImGui::SetWindowSize(EditorPanels::GraffitiDesigner, ImVec2(1150, 720));
        }),
        Wait(3),
        // A new piece from the Designer's own button.
        Click(ByLabel("New", kDesigner)),
        Until("the new piece is open", [](Ctx& c) { GraffitiDesigner* g = GA::G(c.E); return g && g->Piece; }),
        Do("remember the piece", [](Ctx& c) { g_State->PiecePath = GA::G(c.E)->Piece->Path; }),
        ExpectTrue("a .graffiti on disk with one spray layer", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            return fs::exists(fs::u8path(g->Piece->Path)) && g->Piece->Layers.size() == 1 &&
                   g->Piece->Layers[0].Type == Graffiti::LayerType::Spray;
        }),
        // Layers from the Add menu: letters, a gradient fill clipped into them, a stamp.
        Click(ByLabel("Add", kDesigner)), Click(MenuItem("Text")), Wait(3),
        Click(ByLabel("Add", kDesigner)), Click(MenuItem("Fill / gradient")), Wait(3),
        Click(ByLabel("Add", kDesigner)), Click(MenuItem("Stamp")), Wait(3),
        Expect("four layers: spray, text, fill, stamp", [](Ctx& c) {
            const auto& L = GA::G(c.E)->Piece->Layers;
            if (L.size() != 4) return "layer count " + std::to_string(L.size());
            using T = Graffiti::LayerType;
            const bool ok = L[0].Type == T::Spray && L[1].Type == T::Text && L[2].Type == T::Fill && L[3].Type == T::Stamp;
            return ok ? std::string() : std::string("unexpected layer order");
        }),
        Do("the letters read KING, the fill is a gradient inside them, the stamp sits low right", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            auto& L = g->Piece->Layers;
            L[1].Text = "KING";
            L[1].Size = 0.55f;
            L[1].Color = Graffiti::Quantise({0.96f, 0.86f, 0.18f});
            L[2].Clip = true;
            L[2].Gradient = true;
            L[2].Color = Graffiti::Quantise({0.98f, 0.55f, 0.1f});
            L[2].GradientTo = Graffiti::Quantise({0.9f, 0.16f, 0.12f});
            L[2].Opacity = 1.0f;
            L[3].Position = {0.82f, 0.72f};
            L[3].Size = 0.3f;
            g->Selected = 0; // spray into the bottom (spray) layer
            g->Dirty = g->NeedsBuild = true;
        }),
        Wait(5),
        // A stroke on the canvas, across the letters.
        Move("to the canvas", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            return ImVec2(g->CanvasMin.x + (g->CanvasMax.x - g->CanvasMin.x) * 0.15f, g->CanvasMin.y + (g->CanvasMax.y - g->CanvasMin.y) * 0.8f);
        }),
        Wait(2), MouseButton(0, true), Wait(2),
        Glide(AtPoint([](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            return ImVec2(g->CanvasMin.x + (g->CanvasMax.x - g->CanvasMin.x) * 0.85f, g->CanvasMin.y + (g->CanvasMax.y - g->CanvasMin.y) * 0.2f);
        }), 40),
        Wait(3), MouseButton(0, false), Wait(3),
        Expect("the canvas stroke is in the recipe", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            const size_t n = g->Piece->Layers[0].Strokes.size();
            if (n != 1) return "spray layer has " + std::to_string(n) + " strokes";
            auto again = Graffiti::Piece::Load(g->Piece->Path);
            return again && again->Layers.size() == 4 && again->Layers[0].Strokes.size() == 1 ? std::string()
                                                                                                : std::string("not saved to the file");
        }),
        CleanShot("designer", {kDesigner}),
        // Spray in Scene view: the piece goes on the wall at the viewport's centre, and the stroke into it.
        Click(ByLabel("Spray in Scene view", kDesigner)), Wait(2),
        ExpectTrue("viewport spraying is on", [](Ctx& c) { return GA::G(c.E)->ViewportSpray; }),
        Do("tuck the Designer out of the Scene view's way", [](Ctx&) { ImGui::SetWindowCollapsed(EditorPanels::GraffitiDesigner, true); }),
        Wait(3),
        Move("to the viewport's centre", [](Ctx& c) { const ImVec2 m = ViewportCentre(c.E); return ImVec2(m.x - 120.0f, m.y + 30.0f); }),
        Wait(3), MouseButton(0, true), Wait(3),
        Glide(AtPoint([](Ctx& c) { const ImVec2 m = ViewportCentre(c.E); return ImVec2(m.x + 120.0f, m.y - 25.0f); }), 45),
        Wait(4), MouseButton(0, false), Wait(2),
        Expect("a decal entity shows the piece", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            if (g->Target == entt::null || !c.W.Registry.valid(g->Target)) return std::string("no target decal");
            const auto* d = c.W.Registry.try_get<DecalComponent>(g->Target);
            if (!d) return std::string("target has no Decal component");
            if (AssetDatabase::PathKey(d->AssetPath) != AssetDatabase::PathKey(g->Piece->DecalPath()))
                return "decal shows '" + d->AssetPath + "'";
            return g->Piece->Path == g_State->PiecePath ? std::string() : std::string("a different piece was placed");
        }),
        ExpectTrue("the wall stroke was recorded into the same spray layer", [](Ctx& c) {
            return GA::G(c.E)->Piece->Layers[0].Strokes.size() == 2;
        }),
        UntilSecs("the stroke's bake lands", [](Ctx& c) { return BakeIdle(c) && GA::G(c.E)->Bakes >= 2; }, 60),
        Do("turn viewport spraying off, bring the Designer back", [](Ctx& c) {
            GA::G(c.E)->ViewportSpray = false;
            ImGui::SetWindowCollapsed(EditorPanels::GraffitiDesigner, false);
        }),
        Wait(3),
        // Bake twice from the button: byte-identical.
        Do("note the bake count", [](Ctx& c) { g_State->BakesBefore = GA::G(c.E)->Bakes; }),
        Click(ByLabel("Bake", kDesigner)),
        UntilSecs("bake A lands", [](Ctx& c) { return BakeIdle(c) && GA::G(c.E)->Bakes == g_State->BakesBefore + 1; }, 60),
        Do("keep bake A's files", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            const char* maps[] = {"Albedo", "Normal", "Surface", "Aux"};
            for (int i = 0; i < 4; ++i) g_State->Bake[i] = ReadAll(g->Piece->MapPath(maps[i]));
            g_State->Hash = g->LastBakeHash;
        }),
        Click(ByLabel("Bake", kDesigner)),
        UntilSecs("bake B lands", [](Ctx& c) { return BakeIdle(c) && GA::G(c.E)->Bakes == g_State->BakesBefore + 2; }, 60),
        Expect("bake B is byte-identical to bake A", [](Ctx& c) {
            GraffitiDesigner* g = GA::G(c.E);
            const char* maps[] = {"Albedo", "Normal", "Surface", "Aux"};
            for (int i = 0; i < 4; ++i) {
                const std::string now = ReadAll(g->Piece->MapPath(maps[i]));
                if (g_State->Bake[i].size() < 100) return std::string(maps[i]) + " map missing or empty";
                if (now != g_State->Bake[i]) return std::string(maps[i]) + " map differs between bakes";
            }
            if (g->LastBakeHash != g_State->Hash) return std::string("bake hashes differ");
            std::printf("    [graffiti] bakes A and B identical: hash %016llx, albedo %zu bytes\n", (unsigned long long)g->LastBakeHash,
                        g_State->Bake[0].size());
            return std::string();
        }),
        // The decal on the wall: resident, drawn, and on camera.
        Do("hide the Designer", [](Ctx& c) { GA::Shown(c.E) = false; }),
        UntilSecs("the baked decal is resident", [](Ctx& c) {
            DecalSystem* ds = DecalSystem::Current();
            return ds && ds->Resident(GA::G(c.E)->Piece->DecalPath());
        }, 60),
        WaitSeconds(0.5),
        ExpectTrue("decals are drawn", [](Ctx&) { return DecalSystem::Current() && DecalSystem::Current()->VisibleCount() > 0; }),
        CleanShot("decal on the wall", {"###Scene"}),
    }, [](Ctx& c) {
        // Leave nothing behind: the piece, its bake and their metas.
        if (GraffitiDesigner* g = GA::G(c.E)) {
            g->ViewportSpray = false;
            if (g->Piece && g->Piece->Path == (g_State ? g_State->PiecePath : std::string())) {
                std::error_code ec;
                const std::string piece = g->Piece->Path, decal = g->Piece->DecalPath(), dir = g->Piece->BakeDir();
                g->Piece.reset();
                for (const std::string& f : {piece, piece + ".meta", decal, decal + ".meta"}) fs::remove(fs::u8path(f), ec);
                fs::remove_all(fs::u8path(dir), ec);
            }
        }
        GA::Shown(c.E) = false;
    });
}
