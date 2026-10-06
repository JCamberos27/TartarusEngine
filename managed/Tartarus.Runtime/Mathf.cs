namespace Tartarus;

public static class Mathf
{
    public const float PI = MathF.PI, Deg2Rad = MathF.PI / 180, Rad2Deg = 180 / MathF.PI;
    public static float Clamp(float value, float min, float max) => Math.Clamp(value, min, max);
    public static float Clamp01(float value) => Clamp(value, 0, 1);
    public static float Lerp(float from, float to, float t) => from + (to - from) * Clamp01(t);
    public static float LerpUnclamped(float from, float to, float t) => from + (to - from) * t;
    public static float InverseLerp(float from, float to, float value) => from == to ? 0 : Clamp01((value - from) / (to - from));
    public static float MoveTowards(float value, float target, float maxDelta) => MathF.Abs(target - value) <= Math.Max(0, maxDelta) ? target : value + MathF.CopySign(Math.Max(0, maxDelta), target - value);
    public static float Repeat(float value, float length) => length > 0 ? value - MathF.Floor(value / length) * length : 0;
    public static float DeltaAngle(float from, float to) { float delta = Repeat(to - from, 360); return delta > 180 ? delta - 360 : delta; }
    public static float LerpAngle(float from, float to, float t) => from + DeltaAngle(from, to) * Clamp01(t);
    public static bool Approximately(float a, float b) => MathF.Abs(a - b) <= 1e-6f * Math.Max(1, Math.Max(MathF.Abs(a), MathF.Abs(b)));
    public static float Damp(float value, float target, float sharpness, float dt) => Lerp(value, target, 1 - MathF.Exp(-Math.Max(0, sharpness) * Math.Max(0, dt)));
}
public sealed class RandomSource(int seed)
{
    readonly System.Random random = new(seed);
    public float Value => random.NextSingle();
    public int Range(int minInclusive, int maxExclusive) => random.Next(minInclusive, maxExclusive);
    public float Range(float minInclusive, float maxInclusive) => Mathf.Lerp(minInclusive, maxInclusive, Value);
}
