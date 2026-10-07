#pragma once

#include "ComponentReflection.h"

#include <cstring>

// vInspector attributes (Editor Enhancers Phase 3b) for reflected components: a fluent way to
// set ReflectField's attribute members at a registration site, and the one function the
// Inspector uses to resolve them.
//
//   m.Fields = {
//       Field("Speed", T::Float, TARTARUS_REFLECT_FIELD(Mover, Speed), 0.1f, "Metres per second.")
//           .Range(0.0f, 20.0f).Tab("Motion").OnChanged(&ClampSpeed),
//       Field("Debug Id", T::Int, TARTARUS_REFLECT_FIELD(Mover, DebugId)).ReadOnly().NonSerialized(),
//   };
//
// or, for a field already in the list:  Attr(m, "Max Distance").EnableIf("3D Sound", 1);
//
// Variants take static arrays:  static const float kSpeeds[] = {1, 5, 10};  .Variants(kSpeeds, 3)

// The chainable setters, shared by the by-reference builder (Attr) and the by-value one (Field).
template <class Self>
class ReflectFieldSetters {
public:
    Self& Tooltip(const char* t)            { F().Tooltip = t; return S(); }
    Self& Range(float mn, float mx)         { F().Min = mn; F().Max = mx; return S(); }
    Self& Slider(bool logarithmic = false)  { F().Slider = true; F().Logarithmic = logarithmic; return S(); }
    Self& Format(const char* fmt)           { F().Format = fmt; return S(); }
    Self& Enum(const char* labels, int count) { F().EnumLabels = labels; F().EnumCount = count; return S(); }
    Self& Asset(ReflectAssetKind kind)      { F().AssetKind = kind; return S(); }
    Self& Group(const char* g)              { F().Group = g; return S(); }
    Self& Tab(const char* t)                { F().Tab = t; return S(); }
    Self& Hidden()                          { F().EditorHidden = true; return S(); }
    Self& ReadOnly()                        { F().ReadOnly = true; return S(); }
    Self& NonSerialized()                   { F().NonSerialized = true; return S(); }
    Self& VisibleIf(const char* field, int value, bool negate = false) {
        F().VisibleIfField = field; F().VisibleIfValue = value; F().VisibleIfNot = negate; return S();
    }
    Self& EnableIf(const char* field, int value, bool negate = false) {
        F().EnableIfField = field; F().EnableIfValue = value; F().EnableIfNot = negate; return S();
    }
    // Hide / grey out on an arbitrary predicate over the component instance.
    Self& ShowIf(bool (*pred)(const void*))     { F().Condition = pred; F().ConditionDisables = false; return S(); }
    Self& EnableWhen(bool (*pred)(const void*)) { F().Condition = pred; F().ConditionDisables = true; return S(); }
    Self& OnChanged(void (*fn)(World&, entt::entity, void*)) { F().OnChanged = fn; return S(); }
    // `values` (and `labels`, N entries or null) must outlive the registry: static arrays.
    Self& Variants(const float* values, int count, const char* const* labels = nullptr) {
        F().Variants = values; F().VariantCount = count; F().VariantLabels = labels; return S();
    }

private:
    Self& S() { return static_cast<Self&>(*this); }
    ReflectField& F() { return static_cast<Self&>(*this).Get(); }
};

// Attributes onto an existing field, by reference.
class ReflectFieldBuilder : public ReflectFieldSetters<ReflectFieldBuilder> {
public:
    explicit ReflectFieldBuilder(ReflectField& f) : m_F(&f) {}
    ReflectField& Get() { return *m_F; }

private:
    ReflectField* m_F;
};

// A new field built by value; converts implicitly to ReflectField, for field lists.
class ReflectFieldValue : public ReflectFieldSetters<ReflectFieldValue> {
public:
    ReflectFieldValue(const char* name, ReflectFieldType type, void* (*address)(void*),
                      float dragSpeed = 0.1f, const char* tooltip = nullptr) {
        m_Field.Name = name; m_Field.Type = type; m_Field.Address = address;
        m_Field.DragSpeed = dragSpeed; m_Field.Tooltip = tooltip;
    }
    ReflectField& Get() { return m_Field; }
    operator ReflectField() const { return m_Field; } // implicit by design: `m.Fields = { Field(...), ... }`

private:
    ReflectField m_Field;
};

inline ReflectFieldValue Field(const char* name, ReflectFieldType type, void* (*address)(void*),
                               float dragSpeed = 0.1f, const char* tooltip = nullptr) {
    return ReflectFieldValue(name, type, address, dragSpeed, tooltip);
}

// Attributes onto a field already in `meta` (by display Name). Asserts nothing: an unknown name
// returns a builder over a throwaway field, so a typo can't crash static registration.
inline ReflectFieldBuilder Attr(ReflectComponent& meta, const char* name) {
    for (ReflectField& f : meta.Fields)
        if (std::strcmp(f.Name, name) == 0) return ReflectFieldBuilder(f);
    static ReflectField sink;
    sink = ReflectField{};
    return ReflectFieldBuilder(sink);
}

// --- Inspector resolution ---------------------------------------------------------------------
struct ReflectFieldState {
    bool Visible = true;  // drawn at all
    bool Enabled = true;  // editable (false: greyed out - ReadOnly, EnableIf, or a disabling Condition)
};

// The int value of sibling field `name` (Bool reads 0/1, Int/Enum as stored); false when there
// is no such Bool/Int/Enum field.
inline bool ReflectSiblingInt(const ReflectComponent& meta, const char* name, const void* component, int& out) {
    if (!name || !component) return false;
    for (const ReflectField& s : meta.Fields) {
        if (std::strcmp(s.Name, name) != 0) continue;
        void* p = s.Address(const_cast<void*>(component));
        switch (s.Type) {
            case ReflectFieldType::Bool: out = *static_cast<const bool*>(p) ? 1 : 0; return true;
            case ReflectFieldType::Int:
            case ReflectFieldType::Enum: out = *static_cast<const int*>(p); return true;
            default: return false;
        }
    }
    return false;
}

// Resolves VisibleIf, EnableIf, ReadOnly and Condition for one field of one component instance.
// A sibling that doesn't exist leaves the field visible and enabled. EditorHidden is NOT folded
// in (that field is drawn by a custom extra, not hidden from the user).
inline ReflectFieldState EvaluateFieldState(const ReflectComponent& meta, const ReflectField& f, const void* component) {
    ReflectFieldState st;
    int v = 0;
    if (f.VisibleIfField && ReflectSiblingInt(meta, f.VisibleIfField, component, v))
        st.Visible = f.VisibleIfNot ? (v != f.VisibleIfValue) : (v == f.VisibleIfValue);
    if (f.EnableIfField && ReflectSiblingInt(meta, f.EnableIfField, component, v))
        st.Enabled = f.EnableIfNot ? (v != f.EnableIfValue) : (v == f.EnableIfValue);
    if (f.Condition && component && !f.Condition(component)) {
        if (f.ConditionDisables) st.Enabled = false;
        else st.Visible = false;
    }
    if (f.ReadOnly) st.Enabled = false;
    return st;
}
