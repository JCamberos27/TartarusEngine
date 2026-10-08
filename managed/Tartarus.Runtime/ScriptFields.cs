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
/// <summary>A draggable hierarchy reference, serialized relative to the script's object for prefab instances.</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class SceneReferenceAttribute(string component = "Transform", bool childrenOnly = false) : Attribute
{ public string Component { get; } = component; public bool ChildrenOnly { get; } = childrenOnly; }
/// <summary>A list of draggable audio assets, stored as semicolon-separated project paths.</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class SoundReferencesAttribute : Attribute { }
[AttributeUsage(AttributeTargets.Field)] public sealed class InspectorChoicesAttribute(params string[] choices) : Attribute
{ public string[] Choices { get; } = choices; }

// --- vInspector attributes (Editor Enhancers Phase 3b) -------------------------------------------
/// <summary>A button in the Inspector that calls this parameterless instance method. Outside Play
/// it runs on a temporary instance holding the saved field values, and whatever the method changes
/// in serialized fields is saved back (one undo step).</summary>
[AttributeUsage(AttributeTargets.Method)] public sealed class ButtonAttribute(string? label = null) : Attribute
{ public string? Label { get; } = label; }
/// <summary>Consecutive fields with the same foldout name draw inside one collapsible section.</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class FoldoutAttribute(string name) : Attribute
{ public string Name { get; } = name; }
/// <summary>Fields (and buttons) sharing a tab name draw under one tab of a tab bar.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Method)] public sealed class TabAttribute(string name) : Attribute
{ public string Name { get; } = name; }
/// <summary>Shows a property or non-serialized field's current value, read-only.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)] public sealed class ShowInInspectorAttribute : Attribute { }
/// <summary>Calls the named parameterless method after the field is edited in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class OnValueChangedAttribute(string method) : Attribute
{ public string Method { get; } = method; }
/// <summary>Shown greyed out; still saved.</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class ReadOnlyAttribute : Attribute { }
/// <summary>Quick-pick values shown as chips under the field.</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class VariantsAttribute(params object[] values) : Attribute
{ public object[] Values { get; } = values; }
/// <summary>Hides the field while the named field equals `value` (default true).</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class HideIfAttribute(string field, object? value = null) : Attribute
{ public string Field { get; } = field; public object Value { get; } = value ?? true; }
/// <summary>Greys the field out while the named field equals `value` (default true).</summary>
[AttributeUsage(AttributeTargets.Field)] public sealed class DisableIfAttribute(string field, object? value = null) : Attribute
{ public string Field { get; } = field; public object Value { get; } = value ?? true; }

internal static class ScriptFields
{
    static readonly JsonSerializerOptions options = new() { IncludeFields = true };
    static string Scalar(Type type) => type == typeof(float) ? "float" : type == typeof(int) ? "int" :
        type == typeof(bool) ? "bool" : type == typeof(string) ? "string" : type == typeof(Vector3) ? "vec3" :
        type == typeof(AssetReference) ? "asset-ref" :
        type.IsEnum && Enum.GetUnderlyingType(type) == typeof(int) ? "enum" : "unsupported";
    static string InspectorKind(FieldInfo field) => field.FieldType == typeof(Vector3) && field.IsDefined(typeof(ColorAttribute)) ? "color" :
        field.FieldType != typeof(string) ? Kind(field.FieldType) :
        field.IsDefined(typeof(SceneReferenceAttribute)) ? "scene-ref" :
        field.IsDefined(typeof(SoundReferencesAttribute)) ? "sound-refs" :
        field.IsDefined(typeof(InspectorChoicesAttribute)) ? "choice" : "string";
    // Dictionary<string, float|int|bool|string> edits as a key/value list ("dict").
    static Type? DictValue(Type type) =>
        type.IsGenericType && type.GetGenericTypeDefinition() == typeof(Dictionary<,>) && type.GetGenericArguments()[0] == typeof(string) &&
        Scalar(type.GetGenericArguments()[1]) is "float" or "int" or "bool" or "string" ? type.GetGenericArguments()[1] : null;
    static string Kind(Type type) => DictValue(type) != null ? "dict" : Scalar(type);
    static IEnumerable<FieldInfo> Fields(Type type)
    {
        for (Type? t = type; t != null && t != typeof(Script) && t != typeof(Component); t = t.BaseType)
            foreach (var f in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                if (!f.IsInitOnly && !f.IsStatic && !f.IsDefined(typeof(NonSerializedAttribute)) &&
                    (f.IsPublic || f.IsDefined(typeof(SerializeFieldAttribute)))) yield return f;
    }
    const BindingFlags Members = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    static IEnumerable<MethodInfo> Buttons(Type type) => type.GetMethods(Members)
        .Where(m => m.IsDefined(typeof(ButtonAttribute)) && m.GetParameters().Length == 0 && !m.IsGenericMethodDefinition);
    static IEnumerable<MemberInfo> Shows(Type type) => type.GetMembers(Members)
        .Where(m => m.IsDefined(typeof(ShowInInspectorAttribute)) &&
                    (m is FieldInfo || m is PropertyInfo { CanRead: true } p && p.GetIndexParameters().Length == 0));
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
    static object? Condition(string? field, object? value) => field == null ? null : new { field, value = value is Enum ? Convert.ToInt32(value) : value };
    public static object Describe(Type type)
    {
        var defaults = (Script)Activator.CreateInstance(type)!;
        return new {
            @class = type.FullName,
            fields = Fields(type).Where(f => !f.IsDefined(typeof(HideInInspectorAttribute))).Select(f => new {
                name = f.Name, kind = InspectorKind(f), @default = Kind(f.FieldType) == "unsupported" ? null : f.GetValue(defaults),
                component = f.GetCustomAttribute<SceneReferenceAttribute>()?.Component ?? "",
                childrenOnly = f.GetCustomAttribute<SceneReferenceAttribute>()?.ChildrenOnly ?? false,
                choices = f.GetCustomAttribute<InspectorChoicesAttribute>()?.Choices ?? [],
                extensions = f.GetCustomAttribute<AssetPathAttribute>()?.Extensions ?? [],
                labels = Kind(f.FieldType) == "enum" ? Enum.GetNames(f.FieldType) : [],
                values = Kind(f.FieldType) == "enum" ? Enum.GetValues(f.FieldType).Cast<object>().Select(Convert.ToInt32).ToArray() : [],
                min = f.GetCustomAttribute<RangeAttribute>()?.Min, max = f.GetCustomAttribute<RangeAttribute>()?.Max,
                tooltip = f.GetCustomAttribute<TooltipAttribute>()?.Text ?? "",
                header = f.GetCustomAttribute<HeaderAttribute>()?.Text ?? "",
                valueKind = DictValue(f.FieldType) is { } v ? Scalar(v) : "",
                foldout = f.GetCustomAttribute<FoldoutAttribute>()?.Name ?? "",
                tab = f.GetCustomAttribute<TabAttribute>()?.Name ?? "",
                readOnly = f.IsDefined(typeof(ReadOnlyAttribute)),
                onChanged = f.GetCustomAttribute<OnValueChangedAttribute>()?.Method ?? "",
                variants = f.GetCustomAttribute<VariantsAttribute>()?.Values.Select(x => x is Enum ? Convert.ToInt32(x) : x).ToArray() ?? [],
                hideIf = Condition(f.GetCustomAttribute<HideIfAttribute>()?.Field, f.GetCustomAttribute<HideIfAttribute>()?.Value),
                disableIf = Condition(f.GetCustomAttribute<DisableIfAttribute>()?.Field, f.GetCustomAttribute<DisableIfAttribute>()?.Value),
            }).ToArray(),
            buttons = Buttons(type).Select(m => new {
                method = m.Name, label = m.GetCustomAttribute<ButtonAttribute>()!.Label ?? m.Name,
                tab = m.GetCustomAttribute<TabAttribute>()?.Name ?? ""
            }).ToArray(),
            shows = Shows(type).Select(m => new { name = m.Name }).ToArray(),
        };
    }
    public static string DescribeJson(Type type) => JsonSerializer.Serialize(Describe(type), options);

    /// <summary>Calls a [Button] / [OnValueChanged] method; true when it ran.</summary>
    public static bool Invoke(Script script, string method)
    {
        var m = script.GetType().GetMethod(method, Members, Type.EmptyTypes);
        if (m == null) return false;
        m.Invoke(script, null);
        return true;
    }
    /// <summary>The [ShowInInspector] members' current values as display text.</summary>
    public static string ShowValues(Script script)
    {
        var values = new Dictionary<string, string>();
        foreach (var m in Shows(script.GetType()))
        {
            object? v;
            try { v = m is FieldInfo f ? f.GetValue(script) : ((PropertyInfo)m).GetValue(script); }
            catch (TargetInvocationException e) { v = "(" + (e.InnerException?.GetType().Name ?? "error") + ")"; }
            values[m.Name] = v switch {
                null => "null",
                float x => x.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
                Vector3 x => string.Create(System.Globalization.CultureInfo.InvariantCulture, $"({x.X:0.###}, {x.Y:0.###}, {x.Z:0.###})"),
                _ => v.ToString() ?? ""
            };
        }
        return JsonSerializer.Serialize(values);
    }
}
