using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Tartarus;

/// <summary>Attach by fully qualified class name with the C# Script component.</summary>
public abstract class Script : Component
{
    internal uint SlotId;
    internal bool IsEnabled = true;
    public uint InstanceId => SlotId;
    public bool enabled { get => IsEnabled; set { NativeRequest r = new() { Entity = Entity, Script = SlotId, Result = value ? 1 : 0 }; if (Engine.Call(51, ref r) != 0) IsEnabled = value; } }
    public virtual void Awake() => OnCreate();
    public virtual void Start() { }
    public virtual void OnEnable() { }
    public virtual void OnDisable() { }
    public virtual void Update() { }
    public virtual void FixedUpdate() { }
    public virtual void LateUpdate() { }
    public virtual void OnCreate() { }
    public virtual void Update(float dt) => Update();
    public virtual void FixedUpdate(float dt) => FixedUpdate();
    public virtual void OnDestroy() { }
    public virtual string SaveState() => ScriptFields.Save(this);
    public virtual void LoadState(string json) => ScriptFields.Apply(this, json);
}

public abstract class MonoBehaviour : Script { }

public interface IGameplay
{
    void Player(ref PlayerFrame frame);
    void Weapon(ref WeaponFrame frame);
    void Shot(ref ShotFrame frame);
}

/// <summary>Main-thread services; entity IDs include EnTT generation bits.</summary>
public static unsafe class Engine
{
    internal static delegate* unmanaged[Cdecl]<int, NativeRequest*, int> Callback;
    internal static int ThreadId;
    internal static void CheckThread()
    {
        if (Callback == null) throw new InvalidOperationException("Native services unavailable");
        if (Environment.CurrentManagedThreadId != ThreadId) throw new InvalidOperationException("Engine services must run on the game thread");
    }
    public static int Call(int operation, ref NativeRequest request)
    {
        CheckThread();
        fixed (NativeRequest* p = &request) return Callback(operation, p);
    }
    public static int TextCall(int op, string text, ref NativeRequest request)
    {
        byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(text + '\0');
        fixed (byte* p = utf8) { request.Text = (nint)p; return Call(op, ref request); }
    }
    public static void Log(string message) { NativeRequest r = default; TextCall(0, message, ref r); }
    public static float Axis(string action) { NativeRequest r = default; TextCall(10, action, ref r); return r.Value; }
    public static bool Button(string action, bool pressed = false) { NativeRequest r = default; return TextCall(pressed ? 12 : 11, action, ref r) != 0; }
    public static Vector3 Position(uint entity) { NativeRequest r = new() { Entity = entity }; if (Call(20, ref r) == 0) throw new InvalidOperationException("Entity has no transform"); return r.A; }
    public static bool SetPosition(uint entity, Vector3 position) { NativeRequest r = new() { Entity = entity, A = position }; return Call(21, ref r) != 0; }
    public static uint Instantiate(string prefab, Vector3 position) { NativeRequest r = new() { A = position }; if (TextCall(22, prefab, ref r) == 0) throw new InvalidOperationException("Prefab could not be instantiated: " + prefab); return r.Entity; }
    public static bool Destroy(uint entity) { NativeRequest r = new() { Entity = entity }; return Call(23, ref r) != 0; }
    public static bool AnimatorTrigger(uint entity, string trigger) { NativeRequest r = new() { Entity = entity }; return TextCall(24, trigger, ref r) != 0; }
    public static bool Raycast(Vector3 origin, Vector3 direction, float distance, out NativeRequest hit, bool record = true) { hit = new() { A = origin, B = direction, Value = distance }; return Call(record ? 25 : 26, ref hit) != 0; }
    public static bool DynamicBodyMass(uint entity, out float mass) { NativeRequest r = new() { Entity = entity }; bool ok = Call(27, ref r) != 0; mass = r.Value; return ok; }
    public static bool ApplyImpulse(uint entity, Vector3 impulse, Vector3 point) { NativeRequest r = new() { Entity = entity, A = impulse, B = point }; return Call(28, ref r) != 0; }
}
