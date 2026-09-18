#pragma once
#include <string>

namespace SystemFonts {
// Full UTF-8 path of `fileName` (e.g. "segoeui.ttf") in the OS font folder, resolved through
// the known-folder API rather than a hard-coded "C:\Windows\Fonts" (#134 — Windows isn't always
// on C:). Returns "" if the font folder can't be resolved or the file isn't there.
std::string Path(const char* fileName);
}
