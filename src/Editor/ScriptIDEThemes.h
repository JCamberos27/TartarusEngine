#pragma once
#include "../../extern/ImGuiColorTextEdit/TextEditor.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace ScriptIDEThemes {
// RGB values are converted to ImGui's packed color order below.
struct Preset {
    const char* Name;
    uint32_t Background,Text,Keyword,String,Number,Comment,Type,Selection;
};
inline constexpr std::array<Preset,12> Presets{{
    {"Tartarus Dark",0x101010,0xd4d4d4,0x569cd6,0xce9178,0xb5cea8,0x6a9955,0x4ec9b0,0x264f78},
    {"Dracula",0x282a36,0xf8f8f2,0xff79c6,0xf1fa8c,0xbd93f9,0x6272a4,0x8be9fd,0x44475a},
    {"Monokai",0x272822,0xf8f8f2,0xf92672,0xe6db74,0xae81ff,0x75715e,0x66d9ef,0x49483e},
    {"Nord",0x2e3440,0xd8dee9,0x81a1c1,0xa3be8c,0xb48ead,0x7b88a1,0x8fbcbb,0x434c5e},
    {"Gruvbox Dark",0x282828,0xebdbb2,0xfb4934,0xb8bb26,0xd3869b,0x928374,0x8ec07c,0x504945},
    {"Solarized Dark",0x002b36,0x839496,0x859900,0x2aa198,0xd33682,0x586e75,0xb58900,0x073642},
    {"One Dark",0x282c34,0xabb2bf,0xc678dd,0x98c379,0xd19a66,0x7f848e,0xe5c07b,0x3e4451},
    {"Tokyo Night",0x1a1b26,0xc0caf5,0xbb9af7,0x9ece6a,0xff9e64,0x737aa2,0x7dcfff,0x33467c},
    {"Catppuccin Mocha",0x1e1e2e,0xcdd6f4,0xcba6f7,0xa6e3a1,0xfab387,0x7f849c,0x89dceb,0x45475a},
    {"GitHub Light",0xffffff,0x24292f,0xcf222e,0x0a3069,0x0550ae,0x6e7781,0x8250df,0xb6e3ff},
    {"Solarized Light",0xfdf6e3,0x657b83,0x859900,0x2aa198,0xd33682,0x93a1a1,0xb58900,0xeee8d5},
    {"High Contrast",0x000000,0xffffff,0x00ffff,0xffff00,0xff80ff,0x90ee90,0x80c0ff,0x004080}
}};
inline ImU32 Color(uint32_t rgb,unsigned alpha=255) {
    return IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,alpha);
}
inline TextEditor::Palette Palette(int index) {
    const auto& t=Presets[std::clamp(index,0,static_cast<int>(Presets.size())-1)];
    auto p=TextEditor::GetDarkPalette();
    auto set=[&](TextEditor::PaletteIndex slot,uint32_t rgb,unsigned alpha=255){p[static_cast<size_t>(slot)]=Color(rgb,alpha);};
    using I=TextEditor::PaletteIndex;
    set(I::Default,t.Text);set(I::Identifier,t.Text);set(I::Punctuation,t.Text);
    set(I::Keyword,t.Keyword);set(I::Preprocessor,t.Keyword);set(I::PreprocIdentifier,t.Type);
    set(I::Number,t.Number);set(I::String,t.String);set(I::CharLiteral,t.String);
    set(I::KnownIdentifier,t.Type);set(I::Comment,t.Comment);set(I::MultiLineComment,t.Comment);
    set(I::TypeName,t.Type);set(I::MethodName,t.String);set(I::PropertyName,t.Type);
    set(I::FieldName,t.Number);set(I::ParameterName,t.Number);set(I::LocalName,t.Text);
    set(I::NamespaceName,t.Type);set(I::EnumMember,t.Number);
    set(I::Background,t.Background);set(I::Cursor,t.Text);set(I::Selection,t.Selection);
    set(I::LineNumber,t.Comment);set(I::CurrentLineFill,t.Text,16);set(I::CurrentLineFillInactive,t.Text,8);
    set(I::CurrentLineEdge,t.Text,40);set(I::ErrorMarker,0xff4040,100);set(I::Breakpoint,0xff4040,90);
    return p;
}
}
