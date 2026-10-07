using Tartarus;
namespace Tartarus.Tests;

public enum AttributeMode { Off = 0, Slow = 1, Fast = 5 }

/// <summary>vInspector (Phase 3b) fixture: one field per attribute the Inspector understands.</summary>
public sealed class AttributeProbe : MonoBehaviour
{
    [Foldout("Motion"), Tab("Tuning"), Variants(1f, 5f, 10f)] public float Speed = 2;
    [Foldout("Motion"), OnValueChanged(nameof(ClampSpeed))] public float MaxSpeed = 8;
    [ReadOnly] public int Hits = 3;
    public bool Advanced;
    [HideIf(nameof(Advanced), false)] public string Note = "only when Advanced";
    [DisableIf(nameof(Mode), AttributeMode.Off)] public float Boost = 1;
    public AttributeMode Mode = AttributeMode.Slow;
    public Dictionary<string, float> Weights = new() { ["head"] = 2f };
    [ShowInInspector] public float Doubled => Speed * 2;
    [ShowInInspector, NonSerialized] public int Transient = 7;

    public void ClampSpeed() { if (Speed > MaxSpeed) Speed = MaxSpeed; }
    [Button("Reset Hits")] public void ResetHits() { Hits = 0; }
    [Button, Tab("Tuning")] public void DoubleSpeed() { Speed *= 2; }
}
