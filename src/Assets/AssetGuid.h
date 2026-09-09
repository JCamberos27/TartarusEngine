#pragma once
#include <cstdint>
#include <functional>
#include <string>

// 64-bit asset identity — matches the engine's uint64 hash vocabulary and fits in a JSON
// string without double-precision loss (which 128-bit UUIDs risk on naïve parsers).
// Stored as 16 lowercase hex chars in .meta files and scene JSON.
struct AssetGuid {
    std::uint64_t Value = 0;

    bool IsValid() const { return Value != 0; }

    // 16 lowercase hex characters, e.g. "3f2a9c1e8b7d4a60".
    std::string ToString() const;

    // Parses a 16-char lowercase hex string; returns an invalid (zero) guid on malformed input.
    static AssetGuid FromString(const std::string& s);

    // Generates a fresh, non-zero GUID using std::random_device seeded mt19937_64.
    static AssetGuid Generate();

    bool operator==(const AssetGuid& o) const noexcept { return Value == o.Value; }
    bool operator!=(const AssetGuid& o) const noexcept { return Value != o.Value; }
};

template<>
struct std::hash<AssetGuid> {
    std::size_t operator()(const AssetGuid& g) const noexcept {
        return std::hash<std::uint64_t>{}(g.Value);
    }
};
