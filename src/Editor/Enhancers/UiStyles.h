#pragma once
#include <json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Editor Enhancers - tab and component-header styles (docs/EDITOR_ENHANCERS.md, "Tabs & Headers").
// The same icon + colour a Hierarchy row or an Asset Browser folder carries, for the two things
// that are neither: the editor's docked panels (their tabs) and the Inspector's component types
// (their section headers).
//
// Project data, like the folder styles: saved to project/editor_ui_styles.json through AtomicFile,
// so the global file undo journals it. Panels are keyed by their stable id (the part after "###"
// in the window title, e.g. "Hierarchy", "Scene"); components by their display name ("Transform",
// "Mesh Renderer", "Animator Controller"). Colours are IM_COL32-packed like the Palette's.
namespace Enhancers {

struct UiStyle {
    std::string   Icon;      // Font Awesome name, "" = the default glyph (headers only)
    std::uint32_t Color = 0; // IM_COL32-packed, 0 = not set
    bool IsEmpty() const { return Icon.empty() && Color == 0; }
    bool operator==(const UiStyle& o) const { return Icon == o.Icon && Color == o.Color; }
};

class UiStyles {
public:
    static UiStyles& Get();

    std::map<std::string, UiStyle> Panels;     // panel id -> its tab's style
    std::map<std::string, UiStyle> Components; // component display name -> its header's style

    // nullptr when nothing is set for that key.
    const UiStyle* Panel(const std::string& id) const;
    const UiStyle* Component(const std::string& name) const;
    // Set (or, for an empty style, clear) one entry and mark the store dirty.
    void SetPanel(const std::string& id, const UiStyle& s);
    void SetComponent(const std::string& name, const UiStyle& s);

    // The shipped "spectrum" look: panels and the common components walked around the palette's
    // hue wheel in order (red, orange, yellow, lime, green, teal, blue, indigo, purple, pink).
    // `extraComponents` extends the component list (e.g. every registered component) - names
    // already in the built-in order keep their place; the rest continue around the wheel.
    static UiStyles Spectrum(const std::vector<std::string>& extraComponents = {});

    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& j);
    void Reset();

    void Load();   // project/editor_ui_styles.json; a missing file is an empty store
    void MarkDirty() { m_Dirty = true; }
    void Flush();  // atomic write if dirty; once per frame
    static std::string Path();

    bool ExportTo(const std::string& path) const;
    bool ImportFrom(const std::string& path); // replaces both maps on success

    static constexpr int kVersion = 1;

private:
    bool m_Dirty = false;
};

} // namespace Enhancers
