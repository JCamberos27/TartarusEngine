using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Tartarus;

internal static class NativeServices
{
    internal static readonly JsonSerializerOptions Json = new() { IncludeFields = true, PropertyNameCaseInsensitive = true };
    internal static T Read<T>(int op, uint entity = uint.MaxValue, object? payload = null)
    {
        NativeRequest r = new() { Entity = entity };
        if (Engine.TextCall(op, payload is string s ? s : JsonSerializer.Serialize(payload, Json), ref r) == 0)
            throw new InvalidOperationException($"Service {op} failed for object {entity}. Check object, component and field names.");
        return JsonSerializer.Deserialize<T>(Marshal.PtrToStringUTF8(r.Text) ?? "null", Json)!;
    }
    internal static void Write(int op, uint entity, object payload)
    {
        NativeRequest r = new() { Entity = entity };
        if (Engine.TextCall(op, payload is string s ? s : JsonSerializer.Serialize(payload, Json), ref r) == 0)
            throw new InvalidOperationException($"Service {op} failed for object {entity}.");
    }
}

/// <summary>Current callback's scene. IDs become invalid when an object is destroyed or the scene is restored.</summary>
public static class Scene
{
    public static GameObject[] GetObjects() => NativeServices.Read<uint[]>(68).Select(id => new GameObject(id)).ToArray();
    public static GameObject? Find(string name)
    {
        NativeRequest r = default;
        return Engine.TextCall(61, name, ref r) != 0 ? new(r.Entity) : null;
    }
    public static GameObject Create(string name, Vector3 position = default)
    {
        NativeRequest r = new() { A = position };
        if (Engine.TextCall(62, name, ref r) == 0) throw new InvalidOperationException("Cannot create object");
        return new(r.Entity);
    }
    public static T[] FindObjectsOfType<T>() where T : Component => GetObjects().SelectMany(o => o.GetComponents<T>()).ToArray();
    public static GameObject[] FindObjectsWithComponent(string component) => GetObjects().Where(o => o.HasNativeComponent(component)).ToArray();
}

/// <summary>All reflected engine component data, including weapon definitions, player tuning, lighting and audio.</summary>
public readonly record struct NativeComponent(GameObject Object, string Type)
{
    public T Get<T>(string field) => NativeServices.Read<T>(70, Object.Id, new { component = Type, field });
    public void Set<T>(string field, T value) => NativeServices.Write(71, Object.Id, new { component = Type, field, value });
}
public sealed record NativeFieldInfo(string Key, string Label, int Kind, float Min, float Max, string Tooltip, string[] EnumLabels)
{
    public string Group { get; init; } = "";
    public bool EditorHidden { get; init; }
    public string VisibleIfField { get; init; } = "";
    public int VisibleIfValue { get; init; }
    public bool VisibleIfNot { get; init; }
}
public sealed record NativeComponentInfo(string Name, string Category, NativeFieldInfo[] Fields);
public static class ComponentCatalog
{
    public static NativeComponentInfo[] GetAll() => NativeServices.Read<NativeComponentInfo[]>(74);
}
public static class Application
{
    public static bool isPlaying { get { NativeRequest r = default; return Engine.Call(78, ref r) != 0; } }
    public static string ResolveProjectPath(string relative) => NativeServices.Read<string>(77, payload: relative);
}
public static class Debug
{
    public static void Log(object? message) => Engine.Log(message?.ToString() ?? "null");
    public static void Assert(bool condition, string message = "Script assertion failed") { if (!condition) throw new InvalidOperationException(message); }
}
