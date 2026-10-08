using System.Numerics;
using System.Text.Json;

namespace Tartarus;

public readonly partial record struct GameObject
{
    public static GameObject Invalid => new(uint.MaxValue);
    public bool IsValid { get { NativeRequest r = new() { Entity = Id }; return Engine.Call(60, ref r) != 0; } }
    public bool activeInHierarchy { get { NativeRequest r = new() { Entity = Id }; return Engine.Call(59, ref r) != 0; } }
    public string name { get => NativeServices.Read<string>(63, Id); set => NativeServices.Write(64, Id, value); }
    public GameObject? parent {
        get { NativeRequest r = new() { Entity = Id }; if (Engine.Call(65, ref r) == 0) throw new InvalidOperationException("Invalid object"); return r.Entity == uint.MaxValue ? null : new(r.Entity); }
        set { NativeRequest r = new() { Entity = Id, Script = value?.Id ?? uint.MaxValue }; if (Engine.Call(66, ref r) == 0) throw new InvalidOperationException("Invalid parent or hierarchy cycle"); }
    }
    public GameObject[] children => NativeServices.Read<uint[]>(67, Id).Select(id => new GameObject(id)).ToArray();
    public string[] nativeComponents => NativeServices.Read<string[]>(69, Id);
    public bool HasNativeComponent(string component) {
        NativeRequest r = new() { Entity = Id }; return Engine.TextCall(75, JsonSerializer.Serialize(new { component }), ref r) != 0;
    }
    public NativeComponent GetNativeComponent(string component) => HasNativeComponent(component) ? new(this, component) : throw new InvalidOperationException("Missing component: " + component);
    /// <summary>Add authored native data in Edit mode. Active physics actors cannot be rebuilt through this operation.</summary>
    public NativeComponent AddNativeComponent(string component) { NativeServices.Write(72, Id, new { component }); return GetNativeComponent(component); }
    public void RemoveNativeComponent(string component) => NativeServices.Write(73, Id, new { component });
    public T AddComponent<T>() where T : ReflectedComponent,new() {
        var component=new T {Entity=Id};AddNativeComponent(component.NativeType);return component;
    }
    /// <summary>Queues a behaviour for construction at the next script tick. Returns its stable object-local slot ID.</summary>
    public uint AddScript<T>() where T : Script, new() {
        NativeRequest r = new() { Entity = Id };
        if (Engine.TextCall(79, typeof(T).FullName!, ref r) == 0) throw new InvalidOperationException("Cannot attach script");
        return r.Script;
    }
    public void RemoveScript(Script script) {
        if (script.Entity != Id) throw new ArgumentException("Script belongs to another object");
        NativeRequest r = new() { Entity = Id, Script = script.InstanceId };
        if (Engine.Call(80, ref r) == 0) throw new InvalidOperationException("Script no longer attached");
    }
}
