using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

public enum FlickerMode { Fluorescent, Tv, Candle }

/// <summary>Drives the Light on this entity: a fluorescent tube that stutters now and then, a TV screen's restless
/// glow, or a candle's fast shimmer. Works around the light's authored intensity and colour, and puts them back when
/// the script stops.</summary>
public sealed class LightFlicker : Script
{
    [Header("Flicker"), Tooltip("Fluorescent: steady with bursts of off/on stutters. Tv: shifting brightness and tint. Candle: fast small noise.")]
    public FlickerMode Mode = FlickerMode.Fluorescent;
    [Header("Flicker"), Range(0.05f, 60), Tooltip("Shortest wait between events, seconds (Fluorescent: between stutter bursts; Tv: between picture changes).")]
    public float MinInterval = 4;
    [Header("Flicker"), Range(0.05f, 60), Tooltip("Longest wait between events, seconds.")]
    public float MaxInterval = 15;
    [Header("Flicker"), Range(0, 1), Tooltip("How far the light dips: 1 = all the way off.")]
    public float Depth = 1;
    [Header("Flicker"), Tooltip("Random seed, so lights with the same settings don't flicker in step.")]
    public int Seed = 1;

    Light? light;
    float baseIntensity, wait, level = 1, target = 1, time;
    Vector3 baseColor, tint = Vector3.One;
    int pulses;
    Random rng = new(1);

    public override void OnCreate()
    {
        light = GetComponent<Light>();
        if (light == null) return;
        baseIntensity = light.intensity;
        baseColor = light.color;
        rng = new Random(Seed);
        wait = Next(MinInterval, MaxInterval);
    }

    public override void Update(float dt)
    {
        if (light == null) return;
        time += dt;
        switch (Mode)
        {
            case FlickerMode.Fluorescent: Fluorescent(dt); break;
            case FlickerMode.Tv: Tv(dt); break;
            case FlickerMode.Candle: Candle(); break;
        }
        light.intensity = baseIntensity * level;
        light.color = baseColor * tint;
    }

    void Fluorescent(float dt)
    {
        wait -= dt;
        if (wait > 0) return;
        if (pulses == 0 && level >= 1)
        {
            pulses = rng.Next(2, 6) * 2; // each pulse is an off then an on
        }
        if (pulses > 0)
        {
            level = level >= 1 ? 1 - Depth * Next(0.6f, 1) : 1;
            pulses--;
            wait = Next(0.03f, 0.12f);
        }
        if (pulses == 0)
        {
            level = 1;
            wait = Next(MinInterval, MaxInterval);
        }
    }

    void Tv(float dt)
    {
        wait -= dt;
        if (wait <= 0)
        {
            // a cut to a new shot: new brightness, slightly different cast
            target = 1 - Depth * Next(0, 0.6f);
            tint = new Vector3(Next(0.8f, 1.05f), Next(0.85f, 1.05f), Next(0.9f, 1.1f));
            wait = Next(MinInterval, MaxInterval);
        }
        level += (target - level) * MathF.Min(1, dt * 18);
    }

    void Candle()
    {
        float n = MathF.Sin(time * 11.3f + Seed) * 0.5f + MathF.Sin(time * 23.7f + Seed * 2) * 0.3f + MathF.Sin(time * 5.1f) * 0.2f;
        level = 1 - Depth * 0.15f * (n * 0.5f + 0.5f);
    }

    float Next(float lo, float hi) => lo + (float)rng.NextDouble() * (hi - lo);

    public override void OnDestroy()
    {
        if (light == null) return;
        light.intensity = baseIntensity;
        light.color = baseColor;
    }
}
