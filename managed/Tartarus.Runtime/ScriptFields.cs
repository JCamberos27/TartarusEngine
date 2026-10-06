using System.Numerics;
using System.Reflection;
using System.Text.Json;

namespace Tartarus;

[AttributeUsage(AttributeTargets.Field)] public sealed class SerializeFieldAttribute : Attribute { }
[AttributeUsage(AttributeTargets.Field)] public sealed class HideInInspectorAttribute : Attribute { }
[AttributeUsage(AttributeTargets.Field)] public sealed class HeaderAttribute(string text) : Attribute { public string Text { get; }=text; }
[AttributeUsage(AttributeTargets.Field)] public sealed class RangeAttribute(float min, float max) : Attribute
{ public float Min { get; } = min; public float Max { get; } = max; }
[AttributeUsage(AttributeTargets.Field)] public sealed class TooltipAttribute(string text) : Attribute
{ public string Text { get; } = text; }

internal static class ScriptFields
{
    static readonly JsonSerializerOptions options = new() { IncludeFields = true };
    static string Kind(Type type) => type == typeof(float) ? "float" : type == typeof(int) ? "int" :
        type == typeof(bool) ? "bool" : type == typeof(string) ? "string" : type == typeof(Vector3) ? "vec3" :
        type.IsEnum && Enum.GetUnderlyingType(type) == typeof(int) ? "enum" : "unsupported";
    static IEnumerable<FieldInfo> Fields(Type type)
    {
        for (Type? t = type; t != null && t != typeof(Script) && t != typeof(Component); t = t.BaseType)
            foreach (var f in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                if (!f.IsInitOnly && !f.IsStatic && !f.IsDefined(typeof(NonSerializedAttribute)) &&
                    (f.IsPublic || f.IsDefined(typeof(SerializeFieldAttribute)))) yield return f;
    }
    public static void Apply(Script script, string json)
    {
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Object) throw new ArgumentException("Script fields must be a JSON object");
        var fields = Fields(script.GetType()).ToDictionary(f => f.Name);
        foreach (var property in doc.RootElement.EnumerateObject())
        {
            // Removed/renamed fields are discarded, as with serialized Unity behaviours.
            if (!fields.TryGetValue(property.Name, out var field) || Kind(field.FieldType) == "unsupported") continue;
            field.SetValue(script, property.Value.Deserialize(field.FieldType, options));
        }
    }
    public static string Save(Script script) => JsonSerializer.Serialize(Fields(script.GetType())
        .Where(f => Kind(f.FieldType) != "unsupported").ToDictionary(f => f.Name, f => f.GetValue(script)), options);
    public static void ApplyChanges(Script script, string previous, string current)
    {
        using var before = JsonDocument.Parse(previous);
        using var after = JsonDocument.Parse(current);
        if (after.RootElement.ValueKind != JsonValueKind.Object) throw new ArgumentException("Script fields must be a JSON object");
        var changed = after.RootElement.EnumerateObject().Where(p => !before.RootElement.TryGetProperty(p.Name, out var old) || old.GetRawText() != p.Value.GetRawText())
            .ToDictionary(p => p.Name, p => p.Value);
        Apply(script, JsonSerializer.Serialize(changed));
        var removed = before.RootElement.EnumerateObject().Where(p => !after.RootElement.TryGetProperty(p.Name, out _)).Select(p => p.Name).ToHashSet();
        if (removed.Count == 0) return;
        var defaults = (Script)Activator.CreateInstance(script.GetType())!;
        foreach (var field in Fields(script.GetType())) if (removed.Contains(field.Name)) field.SetValue(script, field.GetValue(defaults));
    }
    public static object Describe(Type type)
    {
        var defaults = (Script)Activator.CreateInstance(type)!;
        return new { @class = type.FullName, fields = Fields(type).Where(f => !f.IsDefined(typeof(HideInInspectorAttribute))).Select(f => new {
            name = f.Name, kind = Kind(f.FieldType), @default = Kind(f.FieldType) == "unsupported" ? null : f.GetValue(defaults),
            labels = Kind(f.FieldType) == "enum" ? Enum.GetNames(f.FieldType) : [],
            values = Kind(f.FieldType) == "enum" ? Enum.GetValues(f.FieldType).Cast<object>().Select(Convert.ToInt32).ToArray() : [],
            min = f.GetCustomAttribute<RangeAttribute>()?.Min, max = f.GetCustomAttribute<RangeAttribute>()?.Max,
            tooltip = f.GetCustomAttribute<TooltipAttribute>()?.Text ?? "",
            header = f.GetCustomAttribute<HeaderAttribute>()?.Text ?? ""
        }).ToArray() };
    }
    public static string DescribeJson(Type type) => JsonSerializer.Serialize(Describe(type), options);
}
