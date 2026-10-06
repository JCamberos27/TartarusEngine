using System.Numerics;

namespace Tartarus;

public readonly record struct RaycastHit(GameObject Object, Vector3 Point, Vector3 Normal, float Distance);
public static class Physics
{
    public static bool IsActive { get { NativeRequest r = default; return Engine.Call(1, ref r) != 0; } }
    public static bool Raycast(Vector3 origin, Vector3 direction, out RaycastHit hit, float distance = 1000, uint layerMask = uint.MaxValue, bool hitTriggers = false)
        => Cast(90, origin, direction, 0, distance, layerMask, hitTriggers, out hit);
    public static bool SphereCast(Vector3 origin, float radius, Vector3 direction, out RaycastHit hit, float distance = 1000, uint layerMask = uint.MaxValue, bool hitTriggers = false)
        => Cast(91, origin, direction, radius, distance, layerMask, hitTriggers, out hit);
    static bool Cast(int op, Vector3 origin, Vector3 direction, float radius, float distance, uint mask, bool triggers, out RaycastHit hit)
    {
        if (direction.LengthSquared() < 1e-12f || !float.IsFinite(distance) || distance < 0 || radius < 0) { hit = default; return false; }
        NativeRequest r = new() { A = origin, B = Vector3.Normalize(direction), C = new(radius, 0, 0), Value = distance, Script = mask, Result = triggers ? 1 : 0 };
        bool result = Engine.Call(op, ref r) != 0;
        hit = result ? new(new(r.Entity), r.A, r.B, r.Value) : default; return result;
    }
    /// <summary>At most maxResults unique native collider objects, in unspecified order.</summary>
    public static GameObject[] OverlapSphere(Vector3 center, float radius, int maxResults = 128, uint layerMask = uint.MaxValue, bool hitTriggers = false)
    {
        return NativeServices.Read<uint[]>(92, payload: new { x = center.X, y = center.Y, z = center.Z, radius, maxResults, layerMask, hitTriggers })
            .Select(id => new GameObject(id)).ToArray();
    }
}
/// <summary>The existing first-person player's single native capsule; positions are foot positions.</summary>
public static class PlayerCharacter
{
    public static bool Exists { get { NativeRequest r = default; return Engine.Call(2, ref r) != 0; } }
    public static bool Create(float radius, float cylinderHalfHeight, Vector3 feet) { NativeRequest r = new() { A = feet, B = new(radius, cylinderHalfHeight, 0) }; return Engine.Call(3, ref r) != 0; }
    public static Vector3 Feet { get { NativeRequest r = default; Engine.Call(4, ref r); return r.A; } set { NativeRequest r = new() { A = value }; Engine.Call(5, ref r); } }
    public static bool Resize(float cylinderHalfHeight) { NativeRequest r = new() { Value = cylinderHalfHeight }; return Engine.Call(6, ref r) != 0; }
    public static bool Fits(float cylinderHalfHeight) { NativeRequest r = new() { Value = cylinderHalfHeight }; return Engine.Call(7, ref r) != 0; }
    public static int Move(Vector3 displacement, float dt) { NativeRequest r = new() { A = displacement, Value = dt }; return Engine.Call(8, ref r); }
    public static float PlatformYawDelta { get { NativeRequest r = default; Engine.Call(9, ref r); return r.Value; } }
}
