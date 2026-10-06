using System.Numerics;

namespace Tartarus;

/// <summary>Typed views over reflected native data. Structural changes are authored in Edit mode.</summary>
public abstract class ReflectedComponent : Component
{
    public abstract string NativeType { get; }
    public NativeComponent data => new(gameObject, NativeType);
}
public sealed class Camera : ReflectedComponent
{
    public override string NativeType => "Camera";
    public float fieldOfView { get => data.Get<float>("Field of View"); set => data.Set("Field of View", value); }
    public float nearClipPlane { get => data.Get<float>("Near"); set => data.Set("Near", value); }
    public float farClipPlane { get => data.Get<float>("Far"); set => data.Set("Far", value); }
}
public enum LightType { Point, Spot, Directional }
public sealed class Light : ReflectedComponent
{
    public override string NativeType => "Light";
    public LightType type { get => (LightType)data.Get<int>("Type"); set => data.Set("Type", (int)value); }
    public Vector3 color { get => data.Get<Vector3>("Color"); set => data.Set("Color", value); }
    public float intensity { get => data.Get<float>("Intensity"); set => data.Set("Intensity", value); }
    public float range { get => data.Get<float>("Range"); set => data.Set("Range", value); }
}
public sealed class AudioSource : ReflectedComponent
{
    public override string NativeType => "Audio Source";
    public string clip { get => data.Get<string>("Clip"); set => data.Set("Clip", value); }
    public float volume { get => data.Get<float>("Volume"); set => data.Set("Volume", value); }
    public bool loop { get => data.Get<bool>("Loop"); set => data.Set("Loop", value); }
    public bool playOnStart { get => data.Get<bool>("Play On Start"); set => data.Set("Play On Start", value); }
}
public sealed class Collider : ReflectedComponent
{
    public override string NativeType => "Collider";
    public Vector3 halfExtents { get => data.Get<Vector3>("HalfExtents"); set => data.Set("HalfExtents", value); }
}
public sealed class WeaponDefinition : ReflectedComponent
{
    public override string NativeType => "Weapon Definition";
    public string description { get => data.Get<string>("Description"); set => data.Set("Description", value); }
    public string animationSet { get => data.Get<string>("Animation Set"); set => data.Set("Animation Set", value); }
}
public sealed class FirstPersonController : ReflectedComponent
{
    public override string NativeType => "First Person Controller";
}
