using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

/// <summary>Example attachable script: bob any entity without rebuilding the C++ editor.</summary>
public sealed class Bob : Script
{
    [Header("Motion"), Range(0, 5), Tooltip("Vertical bob amplitude in metres.")]
    public float Height = .25f;
    [Header("Motion"), Range(0, 20), Tooltip("Sine wave speed in radians per second.")]
    public float Speed = 2;
    Vector3 origin;
    float time;
    public override void OnCreate() => origin = Engine.Position(Entity);
    public override void Update(float dt) { time += dt; Engine.SetPosition(Entity, origin + Vector3.UnitY * (MathF.Sin(time * Speed) * Height)); }
    public override string SaveState() => System.Text.Json.JsonSerializer.Serialize(new { origin.X, origin.Y, origin.Z, time });
    public override void LoadState(string json)
    {
        using var d = System.Text.Json.JsonDocument.Parse(json);
        origin = new(d.RootElement.GetProperty("X").GetSingle(), d.RootElement.GetProperty("Y").GetSingle(), d.RootElement.GetProperty("Z").GetSingle());
        time = d.RootElement.GetProperty("time").GetSingle();
    }
}
