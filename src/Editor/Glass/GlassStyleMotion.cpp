#include "GlassStyleMotion.h"

#include "GlassMotion.h"
#include "GlassTokens.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <im_anim.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cmath>

namespace Glass {

namespace {

constexpr ImGuiID kStyleFrom   = 0x53544631u; // "STF1": the style a theme switch departs from
constexpr ImGuiID kStyleTo     = 0x53545431u; // "STT1": the theme's freshly built style
constexpr ImGuiID kThemePulse  = 0x53545031u; // "STP1"... a pulse id, not a path id
constexpr ImGuiID kMoodPlay    = 0x4D4F4431u; // "MOD1"
constexpr ImGuiID kMoodFocus   = 0x4D4F4432u;
constexpr ImGuiID kMoodPrefab  = 0x4D4F4433u;

ImGuiStyle  s_Base;             // the active theme's own style (no moods)
bool        s_HaveBase = false;
bool        s_Dirty = true;     // the live style needs rewriting even at rest
StyleMoods  s_Last;             // the moods the live style was last derived with
StyleMoods  s_Now;

ImVec4 Lerp(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
float LerpF(float a, float b, float t) { return a + (b - a) * t; }
ImVec4 Lift(ImVec4 c, float amount) {
    return ImVec4(LerpF(c.x, 1.0f, amount), LerpF(c.y, 1.0f, amount), LerpF(c.z, 1.0f, amount), c.w);
}
bool SameMoods(const StyleMoods& a, const StyleMoods& b) {
    return std::fabs(a.play - b.play) < 1e-4f && std::fabs(a.focus - b.focus) < 1e-4f && std::fabs(a.prefab - b.prefab) < 1e-4f;
}

} // namespace

void ApplyStyleMoods(ImGuiStyle& style, const StyleMoods& m, bool glassTheme) {
    if (m.play <= 0.0f && m.focus <= 0.0f && m.prefab <= 0.0f) return;
    ImVec4* c = style.Colors;
    // Recede (play) and focus dim share one shape; whichever is stronger wins, so play while the
    // viewport has focus doesn't double up.
    auto recede = [&](ImGuiCol idx) {
        ImVec4& v = c[idx];
        if (glassTheme && v.w < 0.999f) {
            const float a = std::fmax(LerpF(v.w, std::fmax(v.w, PlayRecedeBgAlpha), m.play),
                                      LerpF(v.w, std::fmax(v.w, FocusDimBgAlpha), m.focus));
            v.w = a;
        } else {
            const float k = std::fmin(LerpF(1.0f, PlayRecedeDarken, m.play), LerpF(1.0f, FocusDimDarken, m.focus));
            v.x *= k; v.y *= k; v.z *= k;
        }
    };
    recede(ImGuiCol_WindowBg);
    recede(ImGuiCol_ChildBg);
    const float textMix = std::fmax(PlayRecedeText * m.play, FocusDimText * m.focus);
    if (textMix > 0.0f) c[ImGuiCol_Text] = Lerp(c[ImGuiCol_Text], c[ImGuiCol_TextDisabled], textMix);

    if (m.prefab > 0.0f) {
        const float lift = PrefabLift * m.prefab;
        static const ImGuiCol kChrome[] = {ImGuiCol_MenuBarBg, ImGuiCol_TitleBg, ImGuiCol_TitleBgActive, ImGuiCol_Tab,
                                           ImGuiCol_TabSelected, ImGuiCol_TabDimmedSelected, ImGuiCol_Header};
        for (ImGuiCol idx : kChrome) c[idx] = Lift(c[idx], lift);
        c[ImGuiCol_Border].w = std::fmin(1.0f, c[ImGuiCol_Border].w + PrefabBorderLift * m.prefab);
    }
}

void StyleMotionThemeChanged(const ImGuiStyle& before, bool animate) {
    s_Base = ImGui::GetStyle();
    s_HaveBase = true;
    s_Dirty = true;
    ImAnimContext& motion = Motion();
    if (animate && motion.CategoryOn(MotionCategory::ThemeTweens)) {
        iam_style_register(kStyleFrom, before);
        iam_style_register(kStyleTo, s_Base);
        motion.Pulse(kThemePulse, ThemeTween.durationMs * 0.001f);
        ImGui::GetStyle() = before; // the first frame still shows the old look; the tween takes it from there
    }
}

void StyleMotionFrame(bool playing, bool prefabMode, bool viewportFocused, bool glassTheme) {
    if (!s_HaveBase) {
        s_Base = ImGui::GetStyle();
        s_HaveBase = true;
    }
    ImAnimContext& motion = Motion();
    const bool tween = motion.CategoryOn(MotionCategory::ThemeTweens);
    auto mood = [&](ImGuiID id, bool on, const MotionSpec& in) {
        const float target = on ? 1.0f : 0.0f;
        return tween ? motion.Animate(id, target, 0.0f, in, MoodOut) : target;
    };
    s_Now.play   = mood(kMoodPlay, playing, PlayMoodIn);
    s_Now.focus  = mood(kMoodFocus, viewportFocused && !playing, MoodIn);
    s_Now.prefab = mood(kMoodPrefab, prefabMode, MoodIn);

    const bool themeMoving = motion.PulseActive(kThemePulse);
    if (!themeMoving && !s_Dirty && SameMoods(s_Now, s_Last)) return; // at rest: the style is already right

    ImGuiStyle& style = ImGui::GetStyle();
    if (themeMoving) {
        const float p = motion.PulseProgress(kThemePulse);
        if (p >= 1.0f || !tween) style = s_Base;
        else iam_style_blend_to(kStyleFrom, kStyleTo, EaseProgress(ThemeTween, p), &style, iam_col_oklab);
    } else {
        style = s_Base;
    }
    ApplyStyleMoods(style, s_Now, glassTheme);
    s_Last = s_Now;
    s_Dirty = motion.PulseActive(kThemePulse); // one more rewrite after the tween's last frame
}

StyleMoods CurrentStyleMoods() { return s_Now; }

bool StyleMotionActive() {
    return Motion().PulseActive(kThemePulse) || !SameMoods(s_Now, s_Last) ||
           Motion().Live(kMoodPlay) || Motion().Live(kMoodFocus) || Motion().Live(kMoodPrefab);
}

} // namespace Glass
