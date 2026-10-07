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
    // vInspector: [OnValueChanged] methods to run once this draw's edits are in (EditorHost).
    internal readonly List<string> PendingMethods = new();
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
                // vInspector: a [Foldout] (else a [Tab]) names the group a custom layout shows it under.
                string own=Str(f,"foldout");if(own.Length==0)own=Str(f,"tab");
                return new SerializedProperty(this,f,own.Length>0?own:group);
            }).ToArray();
        }
    }
    public SerializedProperty? FindProperty(string name) => Properties.FirstOrDefault(p => p.Name == name || p.DisplayName == name);
    internal static string Str(JsonElement e,string key) => e.TryGetProperty(key,out var v) && v.ValueKind==JsonValueKind.String ? v.GetString()! : "";
    internal JsonNode? Read(SerializedProperty property) => values[property.Name] ?? property.DefaultValue;
    internal JsonNode? Read(string name) => values[name] ?? FindProperty(name)?.DefaultValue;
    internal void ReplaceAll(string json) {
        var next=JsonNode.Parse(json)!.AsObject();
        values.Clear();
        foreach(var (k,v) in next.ToArray()) { next.Remove(k); values[k]=v; }
        HasModifiedProperties=true;
    }
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
    public string Name { get; }
    public string DisplayName { get; }
    public string Group { get; }
    public string Tooltip { get; }
    public string Kind { get; }
    public float? Min { get; }
    public float? Max { get; }
    public string[] EnumLabels { get; } = [];
    public int[] EnumValues { get; } = [];
    // vInspector attributes (script fields; see ScriptFields.Describe).
    public bool ReadOnly { get; }
    public string OnValueChanged { get; } = "";
    public string ValueKind { get; } = "";
    internal JsonNode?[] Variants { get; } = [];
    readonly string hideField="",disableField="";
    readonly JsonNode? hideValue,disableValue;
    bool ConditionMet(string field,JsonNode? want) {
        if(field.Length==0) return false;
        var now=Owner.Read(field);
        if(now is JsonValue a && want is JsonValue b && a.TryGetValue<double>(out var x) && b.TryGetValue<double>(out var y)) return x==y;
        return JsonNode.DeepEquals(now,want);
    }
    /// <summary>False while a [ReadOnly] or [DisableIf] field is greyed out.</summary>
    public bool IsEnabled => !ReadOnly && !ConditionMet(disableField,disableValue);
    public bool IsVisible {
        get {
            if (Native == null) return !ConditionMet(hideField,hideValue);
            if (Native.VisibleIfField.Length == 0) return true;
            int value = new NativeComponent(Owner.Target,Owner.TypeName).Get<int>(Native.VisibleIfField);
            return Native.VisibleIfNot ? value != Native.VisibleIfValue : value == Native.VisibleIfValue;
        }
    }
    internal SerializedProperty(SerializedObject owner, NativeFieldInfo field) {
        Owner=owner;Native=field;Name=field.Key;DisplayName=field.Label;Group=field.Group;Tooltip=field.Tooltip;Kind="native";
    }
    internal SerializedProperty(SerializedObject owner,JsonElement field,string group) {
        Owner=owner;Name=field.GetProperty("name").GetString()!;DisplayName=Nicify(Name);
        Group=group;
        Tooltip=field.TryGetProperty("tooltip",out var tip)?tip.GetString()??"":"";
        Kind=field.GetProperty("kind").GetString()!;DefaultValue=JsonNode.Parse(field.GetProperty("default").GetRawText());
        if(field.TryGetProperty("min",out var min) && min.ValueKind==JsonValueKind.Number) Min=min.GetSingle();
        if(field.TryGetProperty("max",out var max) && max.ValueKind==JsonValueKind.Number) Max=max.GetSingle();
        if(field.TryGetProperty("labels",out var labels)) EnumLabels=labels.EnumerateArray().Select(x=>x.GetString()!).ToArray();
        if(field.TryGetProperty("values",out var enumValues)) EnumValues=enumValues.EnumerateArray().Select(x=>x.GetInt32()).ToArray();
        ReadOnly=field.TryGetProperty("readOnly",out var ro) && ro.ValueKind==JsonValueKind.True;
        OnValueChanged=SerializedObject.Str(field,"onChanged");
        ValueKind=SerializedObject.Str(field,"valueKind");
        if(field.TryGetProperty("variants",out var vs) && vs.ValueKind==JsonValueKind.Array) Variants=vs.EnumerateArray().Select(v=>JsonNode.Parse(v.GetRawText())).ToArray();
        if(field.TryGetProperty("hideIf",out var hi) && hi.ValueKind==JsonValueKind.Object) {
            hideField=SerializedObject.Str(hi,"field");hideValue=hi.TryGetProperty("value",out var hv)?JsonNode.Parse(hv.GetRawText()):JsonValue.Create(true);
        }
        if(field.TryGetProperty("disableIf",out var di) && di.ValueKind==JsonValueKind.Object) {
            disableField=SerializedObject.Str(di,"field");disableValue=di.TryGetProperty("value",out var dv)?JsonNode.Parse(dv.GetRawText()):JsonValue.Create(true);
        }
    }
    internal JsonNode? RawValue => Owner.Read(this);
    internal void SetRaw(JsonNode? value) => Owner.Write(this,value?.DeepClone());
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
        bool disabled=!property.IsEnabled;
        if(disabled) BeginDisabled(true);
        try {
        switch(property.Kind) {
        case "float": {float value=property.GetValue<float>();changed=property.Min is float min && property.Max is float max ? Slider(label,ref value,min,max):FloatField(label,ref value);if(changed) property.SetValue(value);break;}
        case "int": {int value=property.GetValue<int>();changed=IntField(label,ref value);if(changed){if(property.Min is float min && property.Max is float max)value=Math.Clamp(value,(int)min,(int)max);property.SetValue(value);}break;}
        case "bool": {bool value=property.GetValue<bool>();if(Toggle(label,ref value)) property.SetValue(value);break;}
        case "string": {string value=property.GetValue<string>();if(TextField(label,ref value)) property.SetValue(value);break;}
        case "vec3": {Vector3 value=property.GetValue<Vector3>();if(Vector3Field(label,ref value))property.SetValue(value);break;}
        case "enum": {int value=property.GetValue<int>();int selected=Array.IndexOf(property.EnumValues,value);int next=Popup(label,selected,property.EnumLabels);if(next!=selected && next>=0){property.SetValue(property.EnumValues[next]);changed=true;}break;}
        case "dict": changed=DictionaryField(property);break;
        }
        if(property.Tooltip.Length>0) Tooltip(property.Tooltip);
        // [Variants]: one-click values on a row under the field.
        if(!disabled && property.Variants.Length>0) {
            for(int i=0;i<property.Variants.Length;++i) {
                var v=property.Variants[i];
                string text=v is JsonValue jv && jv.TryGetValue<string>(out var s) ? s : v?.ToJsonString() ?? "null";
                if(property.Kind=="enum" && v is JsonValue ev && ev.TryGetValue<int>(out var iv)) {int at=Array.IndexOf(property.EnumValues,iv);if(at>=0)text=property.EnumLabels[at];}
                if(i>0) SameLine();
                if(Button(text+"##"+property.Name)) {property.SetRaw(v);changed=true;}
            }
        }
        } finally { if(disabled) BeginDisabled(false); }
        // [OnValueChanged]: run after this draw's edits are applied (EditorHost.DrawInspector).
        if(changed && property.OnValueChanged.Length>0 && !property.Owner.PendingMethods.Contains(property.OnValueChanged))
            property.Owner.PendingMethods.Add(property.OnValueChanged);
    }
    // Dictionary<string, scalar>: a row per entry (value + remove), then a "New key" row.
    static readonly Dictionary<string,string> newKeys=new();
    static bool DictionaryField(SerializedProperty property) {
        Label(property.DisplayName);
        var dict=(property.RawValue as JsonObject)?.DeepClone() as JsonObject ?? new JsonObject();
        bool changed=false;string? remove=null;
        foreach(var (key,node) in dict.ToArray()) {
            switch(property.ValueKind) {
            case "float": {float v=node?.GetValue<float>()??0;if(FloatField(key+"##"+property.Name,ref v)){dict[key]=v;changed=true;}break;}
            case "int": {int v=node?.GetValue<int>()??0;if(IntField(key+"##"+property.Name,ref v)){dict[key]=v;changed=true;}break;}
            case "bool": {bool v=node?.GetValue<bool>()??false;if(Toggle(key+"##"+property.Name,ref v)){dict[key]=v;changed=true;}break;}
            default: {string v=node?.GetValue<string>()??"";if(TextField(key+"##"+property.Name,ref v)){dict[key]=v;changed=true;}break;}
            }
            SameLine();
            if(Button("x##"+property.Name+"/"+key)) remove=key;
        }
        if(remove!=null){dict.Remove(remove);changed=true;}
        string slot=property.Owner.Target.Id+"/"+property.Name;
        string text=newKeys.TryGetValue(slot,out var t)?t:"";
        if(TextField("New key##"+property.Name,ref text)) newKeys[slot]=text;
        SameLine();
        if(Button("Add##"+property.Name) && text.Length>0 && !dict.ContainsKey(text)) {
            dict[text]=property.ValueKind switch {"float"=>JsonValue.Create(0f),"int"=>JsonValue.Create(0),"bool"=>JsonValue.Create(false),_=>JsonValue.Create("")};
            newKeys[slot]="";changed=true;
        }
        if(changed) property.SetRaw(dict);
        return changed;
    }
    /// <summary>Greys out (true) the widgets until the matching BeginDisabled(false).</summary>
    public static void BeginDisabled(bool disabled) {NativeRequest r=new() {Result=disabled?1:0};Engine.Call(130,ref r);}
    /// <summary>The next widget goes on the same line.</summary>
    public static void SameLine() {NativeRequest r=default;Engine.Call(131,ref r);}
}
