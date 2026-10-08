using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Tartarus.Editor;

/// <summary>Match a reflected native component name or a fully qualified gameplay script class.
/// "*" provides a fallback for attachable C# scripts only.</summary>
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true)]
public sealed class CustomEditorAttribute(string target) : Attribute { public string Target { get; } = target; }

/// <summary>Unity-style Inspector extension loaded from the editor-only assembly.</summary>
public abstract class Editor
{
    public GameObject target { get; internal set; }
    public SerializedObject serializedObject { get; internal set; } = null!;
    public virtual void OnInspectorGUI() => DrawDefaultInspector();
    public void DrawDefaultInspector() { foreach (var property in serializedObject.Properties) EditorGUILayout.PropertyField(property); }
}

/// <summary>Authored Inspector data. Native fields use the engine's original widgets and validation.
/// Script fields are overrides, independent of runtime instances and lifecycle state.</summary>
public sealed class SerializedObject
{
    static NativeComponentInfo[]? nativeCatalog;
    readonly JsonObject values;
    public GameObject Target { get; }
    public string TypeName { get; }
    public bool IsNative { get; }
    public IReadOnlyList<SerializedProperty> Properties { get; }
    public bool HasModifiedProperties { get; internal set; }
    internal SerializedObject(uint entity, JsonElement data)
    {
        Target = new(entity); TypeName = data.GetProperty("type").GetString()!;
        IsNative = data.GetProperty("native").GetBoolean();
        values = IsNative ? new() : JsonNode.Parse(data.GetProperty("values").GetRawText())!.AsObject();
        if (IsNative) {
            var schema = (nativeCatalog ??= ComponentCatalog.GetAll()).Single(c => c.Name == TypeName);
            Properties = schema.Fields.Where(f => !f.EditorHidden).Select(f => new SerializedProperty(this, f)).ToArray();
        } else {
            string group="Settings";
            Properties = data.GetProperty("metadata").GetProperty("fields").EnumerateArray().Select(f => {
                if(f.TryGetProperty("header",out var header) && !string.IsNullOrWhiteSpace(header.GetString())) group=header.GetString()!;
                return new SerializedProperty(this,f,group);
            }).ToArray();
        }
    }
    public SerializedProperty? FindProperty(string name) => Properties.FirstOrDefault(p => p.Name == name || p.DisplayName == name);
    internal JsonNode? Read(SerializedProperty property) => values[property.Name] ?? property.DefaultValue;
    internal void Write(SerializedProperty property, JsonNode? value) { values[property.Name]=value;HasModifiedProperties=true; }
    public void Update() { } // A fresh object is supplied for each Inspector draw.
    public bool ApplyModifiedProperties() => HasModifiedProperties; // Applied atomically by the host at draw end.
    internal string ToJson() => values.ToJsonString();
}

public sealed class SerializedProperty
{
    internal SerializedObject Owner { get; }
    internal NativeFieldInfo? Native { get; }
    internal JsonNode? DefaultValue { get; }
    internal string FieldMetadata { get; } = "{}";
    public string Name { get; }
    public string DisplayName { get; }
    public string Group { get; }
    public string Tooltip { get; }
    public string Kind { get; }
    public float? Min { get; }
    public float? Max { get; }
    public string[] EnumLabels { get; } = [];
    public int[] EnumValues { get; } = [];
    public bool IsVisible {
        get {
            if (Native == null || Native.VisibleIfField.Length == 0) return true;
            int value = new NativeComponent(Owner.Target,Owner.TypeName).Get<int>(Native.VisibleIfField);
            return Native.VisibleIfNot ? value != Native.VisibleIfValue : value == Native.VisibleIfValue;
        }
    }
    internal SerializedProperty(SerializedObject owner, NativeFieldInfo field) {
        Owner=owner;Native=field;Name=field.Key;DisplayName=field.Label;Group=field.Group;Tooltip=field.Tooltip;Kind="native";
    }
    internal SerializedProperty(SerializedObject owner,JsonElement field,string group) {
        FieldMetadata=field.GetRawText();
        Owner=owner;Name=field.GetProperty("name").GetString()!;DisplayName=Nicify(Name);
        Group=group;
        Tooltip=field.TryGetProperty("tooltip",out var tip)?tip.GetString()??"":"";
        Kind=field.GetProperty("kind").GetString()!;DefaultValue=JsonNode.Parse(field.GetProperty("default").GetRawText());
        if(field.TryGetProperty("min",out var min) && min.ValueKind==JsonValueKind.Number) Min=min.GetSingle();
        if(field.TryGetProperty("max",out var max) && max.ValueKind==JsonValueKind.Number) Max=max.GetSingle();
        if(field.TryGetProperty("labels",out var labels)) EnumLabels=labels.EnumerateArray().Select(x=>x.GetString()!).ToArray();
        if(field.TryGetProperty("values",out var enumValues)) EnumValues=enumValues.EnumerateArray().Select(x=>x.GetInt32()).ToArray();
    }
    static string Nicify(string name) => System.Text.RegularExpressions.Regex.Replace(name,"([a-z0-9])([A-Z])","$1 $2").Replace('_',' ');
    public T GetValue<T>() {
        if(Native != null) return new NativeComponent(Owner.Target,Owner.TypeName).Get<T>(Name);
        var node=Owner.Read(this) ?? throw new InvalidOperationException("Missing field value");
        return node.Deserialize<T>(NativeServices.Json)!;
    }
    public void SetValue<T>(T value) {
        if(Native != null) {new NativeComponent(Owner.Target,Owner.TypeName).Set(Name,value);Owner.HasModifiedProperties=true;}
        else Owner.Write(this,JsonSerializer.SerializeToNode(value,NativeServices.Json));
    }
}

public static partial class EditorGUILayout
{
    public static int Tabs(int selected, params string[] labels) => Toolbar(selected, labels);
    public static int Toolbar(int selected, params string[] labels) {
        NativeRequest r=new() {Result=selected};Engine.TextCall(119,JsonSerializer.Serialize(labels),ref r);return r.Result;
    }
    public static bool Foldout(string label,bool initiallyOpen=false) { NativeRequest r=new() {Result=initiallyOpen?1:0};return Engine.TextCall(117,label,ref r)!=0; }
    public static void EndFoldout() {NativeRequest r=default;Engine.Call(118,ref r);}
    public static void HelpBox(string text) {NativeRequest r=default;Engine.TextCall(120,text,ref r);}
    public static void Tooltip(string text) {NativeRequest r=default;Engine.TextCall(122,text,ref r);}
    public static int Popup(string label,int selected,string[] labels) {
        NativeRequest r=new() {Result=selected};Engine.TextCall(121,JsonSerializer.Serialize(new {label,labels}),ref r);return r.Result;
    }
    public static void PropertyField(SerializedProperty property) {
        if(!property.IsVisible) return;
        if(property.Native != null) {
            NativeRequest r=new() {Entity=property.Owner.Target.Id};
            Engine.TextCall(125,JsonSerializer.Serialize(new {component=property.Owner.TypeName,field=property.Name}),ref r);return;
        }
        bool changed=false;string label=property.DisplayName;
        switch(property.Kind) {
        case "asset-ref": {
            NativeRequest r=new() {Entity=property.Owner.Target.Id};
            var value=property.GetValue<AssetReference>();
            if(Engine.TextCall(130,JsonSerializer.Serialize(new {label,value,metadata=JsonSerializer.Deserialize<JsonElement>(property.FieldMetadata)}),ref r)!=0)
                property.SetValue(JsonSerializer.Deserialize<AssetReference>(System.Runtime.InteropServices.Marshal.PtrToStringUTF8(r.Text)!));
            break;
        }
        case "color": {
            NativeRequest r=new() {A=property.GetValue<Vector3>()};
            if(Engine.TextCall(131,label,ref r)!=0)property.SetValue(r.A);
            break;
        }
        case "scene-ref": case "sound-refs": case "choice": {
            NativeRequest r=new() {Entity=property.Owner.Target.Id};
            string value=property.GetValue<string>();
            if(Engine.TextCall(130,JsonSerializer.Serialize(new {label,value,metadata=JsonSerializer.Deserialize<JsonElement>(property.FieldMetadata)}),ref r)!=0)
                property.SetValue(System.Runtime.InteropServices.Marshal.PtrToStringUTF8(r.Text) ?? "");
            break;
        }
        case "float": {float value=property.GetValue<float>();changed=property.Min is float min && property.Max is float max ? Slider(label,ref value,min,max):FloatField(label,ref value);if(changed) property.SetValue(value);break;}
        case "int": {int value=property.GetValue<int>();changed=IntField(label,ref value);if(changed){if(property.Min is float min && property.Max is float max)value=Math.Clamp(value,(int)min,(int)max);property.SetValue(value);}break;}
        case "bool": {bool value=property.GetValue<bool>();if(Toggle(label,ref value)) property.SetValue(value);break;}
        case "string": {string value=property.GetValue<string>();if(TextField(label,ref value)) property.SetValue(value);break;}
        case "vec3": {Vector3 value=property.GetValue<Vector3>();if(Vector3Field(label,ref value))property.SetValue(value);break;}
        case "enum": {int value=property.GetValue<int>();int selected=Array.IndexOf(property.EnumValues,value);int next=Popup(label,selected,property.EnumLabels);if(next!=selected && next>=0)property.SetValue(property.EnumValues[next]);break;}
        }
        if(property.Tooltip.Length>0) Tooltip(property.Tooltip);
    }
}
