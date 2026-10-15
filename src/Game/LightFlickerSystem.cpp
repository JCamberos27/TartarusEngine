#include "LightFlickerSystem.h"
#include "World.h"
#include "Components.h"

#include <algorithm>
#include <cmath>

namespace {
using Mode = LightFlickerComponent::FlickerMode;

float Unit(LightFlickerComponent& f) { // xorshift32, 0..1
    std::uint32_t x = f.Rng ? f.Rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    f.Rng = x;
    return (float)(x >> 8) / 16777216.0f;
}
float Next(LightFlickerComponent& f, float lo, float hi) { return lo + Unit(f) * (hi - lo); }

void Fluorescent(LightFlickerComponent& f, float dt) {
    f.Wait -= dt;
    if (f.Wait > 0.0f) return;
    if (f.Pulses == 0 && f.Level >= 1.0f) f.Pulses = (2 + (int)(Unit(f) * 4.0f) % 4) * 2; // each pulse is an off then an on
    if (f.Pulses > 0) {
        f.Level = f.Level >= 1.0f ? 1.0f - f.Depth * Next(f, 0.6f, 1.0f) : 1.0f;
        --f.Pulses;
        f.Wait = Next(f, 0.03f, 0.12f);
    }
    if (f.Pulses == 0) {
        f.Level = 1.0f;
        f.Wait = Next(f, f.MinInterval, f.MaxInterval);
    }
}

void Tv(LightFlickerComponent& f, float dt) {
    f.Wait -= dt;
    if (f.Wait <= 0.0f) {
        // a cut to a new shot: new brightness, slightly different cast
        f.Target = 1.0f - f.Depth * Next(f, 0.0f, 0.6f);
        f.Tint = {Next(f, 0.8f, 1.05f), Next(f, 0.85f, 1.05f), Next(f, 0.9f, 1.1f)};
        f.Wait = Next(f, f.MinInterval, f.MaxInterval);
    }
    f.Level += (f.Target - f.Level) * std::min(1.0f, dt * 18.0f);
}

void Candle(LightFlickerComponent& f) {
    const float s = (float)f.Seed, t = f.Time;
    const float n = std::sin(t * 11.3f + s) * 0.5f + std::sin(t * 23.7f + s * 2.0f) * 0.3f + std::sin(t * 5.1f) * 0.2f;
    f.Level = 1.0f - f.Depth * 0.15f * (n * 0.5f + 0.5f);
}
}

void UpdateLightFlickers(World& world, float dt) {
    auto view = world.Registry.view<LightFlickerComponent, LightComponent>(entt::exclude<InactiveTag>);
    for (auto entity : view) {
        auto& f = view.get<LightFlickerComponent>(entity);
        auto& light = view.get<LightComponent>(entity);
        if (!f.Started) {
            f.BaseIntensity = light.Intensity;
            f.BaseColor = light.Color;
            f.Rng = (std::uint32_t)f.Seed * 2654435761u + 1u;
            f.Wait = Next(f, f.MinInterval, f.MaxInterval);
            f.Started = true;
        }
        f.Time += dt;
        switch ((Mode)f.Mode) {
            case Mode::Fluorescent: Fluorescent(f, dt); break;
            case Mode::Tv: Tv(f, dt); break;
            case Mode::Candle: Candle(f); break;
        }
        light.Intensity = f.BaseIntensity * f.Level;
        light.Color = f.BaseColor * f.Tint;
    }
}
