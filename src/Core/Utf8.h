#pragma once
#include <cstddef>
#include <string>

// Cuts `s` to at most maxBytes bytes without splitting a UTF-8 code point: the cut backs up over
// continuation bytes (10xxxxxx) so a multi-byte character straddling the limit is dropped whole.
inline void Utf8TruncateBytes(std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
}
