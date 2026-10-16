// Editor smoke suite: Particles. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// A Particle System the way a user makes one: Create > Empty (spawned in front of the editor
// camera), Add Component > Particle System, then the emitter's settings edited in the Inspector
// (typed numbers, the Emitting toggle), its preview driven by the section's Restart / Emit Burst /
// Clear buttons and the Emitting toggle (particles simulate in the editor, not only in Play), the
// Scene viewport shot with it running, and every step undone back to the empty scene.

} // namespace
#include "ParticleSystem.h"
namespace {
namespace smoke_particles {

ParticleSystemComponent* System(Ctx& c) {
    const entt::entity e = ByName(c.W, "Empty");
    return e == entt::null ? nullptr : c.W.Registry.try_get<ParticleSystemComponent>(e);
}
// The emitter's live particle count (-1 without one).
int LiveCount(Ctx& c) {
    const ParticleSystemComponent* ps = System(c);
    return ps ? (int)ps->Live.size() : -1;
}
// A group header of the Particle System section ("Emission", "Lifetime"...), opened unless `field`
// (one of its fields) is already showing - group open state survives between tests.
std::vector<Step> OpenGroup(const char* group, const char* field) {
    const std::string tag = std::string("field:Particle System/") + field;
    return {InspReveal(ByExact(group, kInspector)), ClickUnlessShown(ByExact(group, kInspector), ByTag(tag)),
            Until(std::string(field) + " shown", [tag](Ctx&) { return Find(ByTag(tag)) != nullptr; })};
}
// A Particle System section button (Restart / Emit Burst / Clear).
Query SectionButton(const char* label) { return ByExact(label, kInspector); }

} // namespace smoke_particles

void RegisterParticlesTests() {
    using namespace smoke_particles;
    using ChromeUi::MenuClick;
    using ChromeUi::MenuExact;

    Add("particles: a Particle System in front of the camera: emitter edits, preview, viewport shot, undo", InspCat({
        {FreshScene(), ChromeUi::ResetCamera(), Wait(3),
         Do("Scene tab in front", [](Ctx& c) { c.E.RequestSceneTabFocus(); }), Wait(2)},
        MenuClick("Create", "Cube", MenuExact(ICON_FA_DIAGRAM_PROJECT "  Empty")),
        {Until("Empty created and selected", [](Ctx& c) {
             const entt::entity e = ByName(c.W, "Empty");
             return e != entt::null && X::Selected(c.E) == e;
         }),
         Expect("it is in front of the camera", [](Ctx& c) {
             const glm::vec3 p = c.W.Registry.get<TransformComponent>(ByName(c.W, "Empty")).Position;
             const glm::vec3 to = p - c.Cam.Position;
             const float ahead = glm::dot(to, c.Cam.Front());
             if (ahead <= 0.5f) return "the Empty is " + std::to_string(ahead) + " m along the camera's view";
             if (glm::length(to - c.Cam.Front() * ahead) > ahead) return std::string("the Empty is well off the view axis");
             return std::string();
         }),
         Do("note the undo depth", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); }),
         InspReveal(ByLabel("Add Component", kInspector)),
         Click(ByLabel("Add Component", kInspector)), Wait(3),
         TypeText("Particle System"), Wait(3),
         Click(InspPopup(ICON_FA_FIRE "  Particle System")), Wait(3),
         Until("a Particle System on the Empty", [](Ctx& c) { return System(c) != nullptr; }),
         InspOpen("Particle System"), InspUntilOpen("Particle System"), Wait(3),
         Shot("section", {kInspector})},

        // Emission: Rate (typed), Emitting toggled off and back on.
        OpenGroup("Emission", "Rate"),
        InspTypeInto(ByTag("field:Particle System/Rate"), "150"),
        {ExpectTrue("Rate = 150", [](Ctx& c) { return InspNear(System(c)->Rate, 150.0f); })},
        // Lifetime: Lifetime and Start Size (typed).
        OpenGroup("Lifetime", "Lifetime"),
        InspTypeInto(ByTag("field:Particle System/Lifetime"), "2.5"),
        {ExpectTrue("Lifetime = 2.5", [](Ctx& c) { return InspNear(System(c)->Lifetime, 2.5f); })},
        InspTypeInto(ByTag("field:Particle System/Start Size"), "0.4"),
        {ExpectTrue("Start Size = 0.4", [](Ctx& c) { return InspNear(System(c)->StartSize, 0.4f); }),
         Expect("each edit is one undo entry", [](Ctx& c) {
             const double added = (double)X::UndoDepth(c.E) - g_Number;
             return added >= 4.0 ? std::string() : "only " + std::to_string((int)added) + " new undo entries for 4 edits";
         }),

         // The preview: it emits while editing.
         Until("particles are emitted in the editor", [](Ctx& c) { return LiveCount(c) > 5; }, 600),
         HoverPanel(kScene), WaitSeconds(0.5),
         Shot("viewport", {kScene}),
         InspReveal(SectionButton("Clear")),
         Do("note the live count", [](Ctx& c) { g_Number = LiveCount(c); }),
         Click(SectionButton("Clear")), Wait(1),
         // Still emitting at 150/s: a slow frame (llvmpipe) refills more than a handful after the
         // Clear, so compare with the count before it.
         Expect("Clear empties it", [](Ctx& c) {
             const int n = LiveCount(c);
             return n < 5 || n < g_Number / 4.0 ? std::string() : std::to_string(n) + " live after Clear, " + std::to_string((int)g_Number) + " before";
         }),
         InspReveal(ByTag("field:Particle System/Emitting")), Click(ByTag("field:Particle System/Emitting")), Wait(3),
         ExpectTrue("Emitting off (stop)", [](Ctx& c) { return !System(c)->Emitting; }),
         InspReveal(SectionButton("Clear")), Click(SectionButton("Clear")), WaitSeconds(0.3),
         ExpectTrue("nothing emitted while stopped", [](Ctx& c) { return LiveCount(c) == 0; }),
         InspReveal(SectionButton("Emit Burst")), Click(SectionButton("Emit Burst")), Wait(2),
         ExpectTrue("Emit Burst spawns particles while stopped", [](Ctx& c) { return LiveCount(c) > 0; }),
         InspReveal(ByTag("field:Particle System/Emitting")), Click(ByTag("field:Particle System/Emitting")), Wait(3),
         ExpectTrue("Emitting on again (play)", [](Ctx& c) { return System(c)->Emitting; }),
         InspReveal(SectionButton("Restart")), Click(SectionButton("Restart")), Wait(1),
         Until("emitting again after Restart", [](Ctx& c) { return LiveCount(c) > 5; }, 600),
         HoverPanel(kScene), WaitSeconds(0.3),
         Shot("viewport-restarted", {kScene, kInspector}),

         // Undo everything: the edits, the component, the object.
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo the second Emitting toggle", [](Ctx& c) { return System(c) && !System(c)->Emitting; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo the first Emitting toggle", [](Ctx& c) { return System(c) && System(c)->Emitting; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo Start Size", [](Ctx& c) {
             return System(c) && InspNear(System(c)->StartSize, ParticleSystemComponent{}.StartSize);
         }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo Lifetime", [](Ctx& c) { return System(c) && InspNear(System(c)->Lifetime, ParticleSystemComponent{}.Lifetime); }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo Rate", [](Ctx& c) { return System(c) && InspNear(System(c)->Rate, ParticleSystemComponent{}.Rate); }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo Add Component", [](Ctx& c) { return ByName(c.W, "Empty") != entt::null && !System(c); }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo Create Empty", [](Ctx& c) { return ByName(c.W, "Empty") == entt::null; }),
         Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3), Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("redo brings the emitter back", [](Ctx& c) { return System(c) != nullptr; }),
         Until("and it emits again", [](Ctx& c) { return LiveCount(c) > 0; }, 600)},
    }));
}
