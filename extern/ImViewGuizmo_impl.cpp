// Single translation unit for ImViewGuizmo's implementation, same treatment as
// stb_image_impl.cpp / miniaudio_impl.cpp — the header is include-only everywhere else.
#include <imgui.h>
// The header uses std::min/max/sort/array internally but doesn't include their headers itself
// (relies on them being pulled in transitively elsewhere) — this TU is minimal, so pull them in.
#include <algorithm>
#include <array>
#define IMVIEWGUIZMO_IMPLEMENTATION
#include "ImViewGuizmo.h"
