using System.Numerics;

namespace Tartarus;

public abstract class Component
{
    public uint Entity { get; internal set; }
    public GameObject gameObject => new(Entity);
    public Transform transform => new() { Entity = Entity };
    public T? GetComponent<T>() where T : Component => gameObject.GetComponent<T>();
    public T[] GetComponents<T>() where T : Component => gameObject.GetComponents<T>();
}
public readonly partial record struct GameObject(uint Id)
{
    public Transform transform => new() { Entity = Id };
    public T? GetComponent<T>() where T : Component => GetComponents<T>().FirstOrDefault();
    public T[] GetComponents<T>() where T : Component
    {
        if (typeof(Script).IsAssignableFrom(typeof(T))) return Entry.GetScripts(Id).OfType<T>().ToArray();
        if (typeof(ReflectedComponent).IsAssignableFrom(typeof(T))) {
            var view=(ReflectedComponent)Activator.CreateInstance(typeof(T))!;
            if(!HasNativeComponent(view.NativeType)) return [];
            view.Entity=Id;return [(T)(Component)view];
        }
        int kind = typeof(T) == typeof(Transform) ? 0 : typeof(T) == typeof(Rigidbody) ? 1 : typeof(T) == typeof(Animator) ? 2 : -1;
        NativeRequest r = new() { Entity = Id, Result = kind };
        if (kind < 0 || Engine.Call(40, ref r) == 0) return [];
        Component component = kind == 0 ? new Transform() : kind == 1 ? new Rigidbody() : new Animator();
        component.Entity = Id; return [(T)component];
    }
    public bool activeSelf { get { NativeRequest r = new() { Entity = Id }; return Engine.Call(49, ref r) != 0; } }
    public void SetActive(bool active) { NativeRequest r = new() { Entity = Id, Result = active ? 1 : 0 }; Engine.Call(50, ref r); }
    public static GameObject Instantiate(string prefab, Vector3 position) => new(Engine.Instantiate(prefab, position));
    public static void Destroy(GameObject entity) => Engine.Destroy(entity.Id);
}
public sealed class Transform : Component
{
    Vector3 Get(int op) { NativeRequest r = new() { Entity = Entity }; if (Engine.Call(op, ref r) == 0) throw new InvalidOperationException("Missing Transform"); return r.A; }
    void Set(int op, Vector3 value) { NativeRequest r = new() { Entity = Entity, A = value }; if (Engine.Call(op, ref r) == 0) throw new InvalidOperationException("Missing Transform"); }
    public Vector3 position { get => Get(35); set => Set(36, value); }
    public Vector3 localPosition { get => Get(20); set => Set(21, value); }
    public Vector3 localScale { get => Get(31); set => Set(32, value); }
    public Quaternion localRotation {
        get { NativeRequest r = new() { Entity = Entity }; if (Engine.Call(33, ref r) == 0) throw new InvalidOperationException("Missing Transform"); return new(r.A, r.Value); }
        set { NativeRequest r = new() { Entity = Entity, A = new(value.X, value.Y, value.Z), Value = value.W }; if (Engine.Call(34, ref r) == 0) throw new InvalidOperationException("Invalid Transform rotation"); }
    }
    public Vector3 TransformPoint(Vector3 point) => ConvertPoint(87,point);
    public Vector3 InverseTransformPoint(Vector3 point) => ConvertPoint(88,point);
    Vector3 ConvertPoint(int op,Vector3 point) {
        NativeRequest r=new() {Entity=Entity,A=point};
        if(Engine.Call(op,ref r)==0) throw new InvalidOperationException("Invalid or singular transform");return r.A;
    }
    Vector3 Axis(Vector3 axis) { var direction=TransformPoint(axis)-position;return direction.LengthSquared()>1e-12f?Vector3.Normalize(direction):Vector3.Zero; }
    public Vector3 forward => Axis(-Vector3.UnitZ);
    public Vector3 right => Axis(Vector3.UnitX);
    public Vector3 up => Axis(Vector3.UnitY);
    public void Translate(Vector3 worldDisplacement) => position+=worldDisplacement;
    public void Rotate(Vector3 localAxis,float degrees) {
        if(localAxis.LengthSquared()>1e-12f) localRotation=Quaternion.Normalize(localRotation*Quaternion.CreateFromAxisAngle(Vector3.Normalize(localAxis),degrees*Mathf.Deg2Rad));
    }
}
public enum ForceMode { Force, Impulse, VelocityChange, Acceleration }
public sealed class Rigidbody : Component
{
    public Vector3 velocity {
        get { NativeRequest r = new() { Entity = Entity }; if (Engine.Call(41, ref r) == 0) throw new InvalidOperationException("Missing physics body"); return r.A; }
        set { NativeRequest r = new() { Entity = Entity, A = value }; Engine.Call(42, ref r); }
    }
    public float mass { get { NativeRequest r = new() { Entity = Entity }; Engine.Call(43, ref r); return r.Value; } }
    public void AddForce(Vector3 force, ForceMode mode = ForceMode.Force) { NativeRequest r = new() { Entity = Entity, A = force, Result = (int)mode }; Engine.Call(44, ref r); }
    public void AddImpulseAtPosition(Vector3 impulse, Vector3 point) => Engine.ApplyImpulse(Entity, impulse, point);
}
public sealed class Animator : Component
{
    void Set(int op, string name, float value) { NativeRequest r = new() { Entity = Entity, Value = value }; Engine.TextCall(op, name, ref r); }
    public void SetTrigger(string name) => Engine.AnimatorTrigger(Entity, name);
    public void ResetTrigger(string name) => Set(39, name, 0);
    public void SetFloat(string name, float value) => Set(45, name, value);
    public void SetInteger(string name, int value) => Set(46, name, value);
    public void SetBool(string name, bool value) => Set(47, name, value ? 1 : 0);
    public float GetFloat(string name) { NativeRequest r = new() { Entity = Entity }; Engine.TextCall(48, name, ref r); return r.Value; }
    public int GetInteger(string name) => (int)GetFloat(name);
    public bool GetBool(string name) => GetFloat(name) != 0;
    public string CurrentState => NativeServices.Read<string>(81,Entity);
    public bool HasTag(string tag) {NativeRequest r=new() {Entity=Entity};return Engine.TextCall(82,tag,ref r)!=0;}
    public bool EventFired(string name) {NativeRequest r=new() {Entity=Entity};return Engine.TextCall(83,name,ref r)!=0;}
    public bool InState(string name) {NativeRequest r=new() {Entity=Entity};return Engine.TextCall(84,name,ref r)!=0;}
}
public static class Input
{
    public static float GetAxis(string action) => Engine.Axis(action);
    public static bool GetButton(string action) => Engine.Button(action);
    public static bool GetButtonDown(string action) => Engine.Button(action, pressed: true);
    public static bool GetButtonUp(string action) { NativeRequest r = default; return Engine.TextCall(13, action, ref r) != 0; }
}
public static partial class Time
{
    public static float deltaTime { get; internal set; }
    public static float fixedDeltaTime { get; internal set; }
}
