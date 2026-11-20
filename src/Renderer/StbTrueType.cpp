// stb_truetype (it comes with Dear ImGui as imstb_truetype.h), compiled once for HudText and WorldText. ImGui keeps
// its own static copy inside imgui_draw.cpp; these are the engine's external-linkage stbtt_* functions.
#define STB_TRUETYPE_IMPLEMENTATION
#pragma warning(push, 0)
#include "imstb_truetype.h"
#pragma warning(pop)
