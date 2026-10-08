using System.Numerics;

namespace Tartarus;

public enum AudioBus { SFX, Music, Ambient, UI, Voice }
/// <summary>Generation-tagged native voice. Finished/stopped handles safely become inactive.</summary>
public readonly record struct AudioVoice(uint Id)
{
    public bool IsPlaying { get { NativeRequest r = new() { Entity = Id }; return Engine.Call(96, ref r) != 0; } }
    public void Stop() { NativeRequest r = new() { Entity = Id }; Engine.Call(95, ref r); }
    public void SetVolume(float volume) { NativeRequest r = new() { Entity = Id, Value = volume }; Engine.Call(97, ref r); }
    public void SetPosition(Vector3 position) { NativeRequest r = new() { Entity = Id, A = position }; Engine.Call(98, ref r); }
}
public static class Audio
{
    public static AudioVoice Play(string path, float volume = 1, bool loop = false, AudioBus bus = AudioBus.SFX)
        => PlayInternal(path, volume, loop, bus, false, default, 1, 40);
    public static AudioVoice PlayAtPosition(string path, Vector3 position, float volume = 1, bool loop = false, AudioBus bus = AudioBus.SFX, float minDistance = 1, float maxDistance = 40, float pitch = 1)
        => PlayInternal(path, volume, loop, bus, true, position, minDistance, maxDistance,pitch);
    static AudioVoice PlayInternal(string path, float volume, bool loop, AudioBus bus, bool spatial, Vector3 position, float minDistance, float maxDistance,float pitch=1)
    {
        NativeRequest r = default;
        var payload = new { path, volume, loop, bus = (int)bus, spatial, x = position.X, y = position.Y, z = position.Z, minDistance, maxDistance,pitch };
        if (Engine.TextCall(94, System.Text.Json.JsonSerializer.Serialize(payload), ref r) == 0) throw new InvalidOperationException("Audio playback failed: " + path);
        return new(r.Entity);
    }
}
public sealed record TimeSnapshot(double Time, double UnscaledTime, double Realtime, ulong FrameCount, float UnscaledDeltaTime, float TimeScale);
public static partial class Time
{
    /// <summary>Copy once per callback when reading several clock values; preserves native double precision.</summary>
    public static TimeSnapshot GetSnapshot() => NativeServices.Read<TimeSnapshot>(93);
    public static double time => GetSnapshot().Time;
    public static double unscaledTime => GetSnapshot().UnscaledTime;
    public static double realtimeSinceStartup => GetSnapshot().Realtime;
    public static float unscaledDeltaTime => GetSnapshot().UnscaledDeltaTime;
    public static ulong frameCount => GetSnapshot().FrameCount;
}
