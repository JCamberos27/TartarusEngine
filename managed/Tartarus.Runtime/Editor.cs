using System.Numerics;

namespace Tartarus.Editor;

/// <summary>Editor-only tools discovered in Tartarus.Editor.dll. Native editor and ImGui ownership remain C++.</summary>
public abstract class EditorWindow
{
    internal bool Open;
    public virtual string Title => GetType().Name;
    public virtual void OnEnable() { }
    public virtual void OnDisable() { }
    public virtual void Update(float unscaledDeltaTime) { }
    public virtual void OnSelectionChanged() { }
    public virtual void OnPlayModeChanged(bool playing) { }
    public abstract void OnGUI();
    public virtual string SaveState() => "{}";
    public virtual void LoadState(string json) { }
    public static T GetWindow<T>() where T : EditorWindow => EditorHost.GetWindow<T>();
    public void Close() => Open = false;
}
[AttributeUsage(AttributeTargets.Method)]
public sealed class MenuItemAttribute(string path) : Attribute { public string Path { get; } = path; }
public static class Selection
{
    public static GameObject? activeGameObject {
        get { NativeRequest r = default; return Engine.Call(111, ref r) != 0 ? new(r.Entity) : null; }
        set { NativeRequest r = new() { Entity = value?.Id ?? uint.MaxValue }; if (Engine.Call(112, ref r) == 0) throw new InvalidOperationException("Selection unavailable"); }
    }
}
public static class Undo
{
    /// <summary>Take one scene/asset undo snapshot BEFORE a complete editor command modifies anything.</summary>
    public static void RecordScene(string label) { NativeRequest r = default; if (Engine.TextCall(110, label, ref r) == 0) throw new InvalidOperationException("Undo unavailable"); }
    public static void RecordObject(GameObject target, string label) => RecordScene(label);
    /// <summary>Record a file before an editor tool writes or deletes it.</summary>
    public static void RecordAsset(string path) { NativeRequest r=default; if(Engine.TextCall(126,path,ref r)==0) throw new InvalidOperationException("Asset undo unavailable"); }
    public static void PerformUndo() { NativeRequest r=default;Engine.Call(127,ref r); }
    public static void PerformRedo() { NativeRequest r=default;Engine.Call(128,ref r); }
}
public static class EditorUtility
{
    public static bool IsPlaying => Application.isPlaying;
    public static void SetDirty() { NativeRequest r = default; Engine.Call(113, ref r); }
    public static void RequestScriptCompilation() { NativeRequest r = default; Engine.Call(114, ref r); }
    public static void OpenScript(string path="",int line=1,int column=1) { NativeRequest r=new(){Result=line,Value=column};Engine.TextCall(129,path,ref r); }
}
/// <summary>Balanced native widgets; valid only inside EditorWindow.OnGUI or an editor menu command.</summary>
public static partial class EditorGUILayout
{
    static int Widget(int op, string label, ref NativeRequest r) => Engine.TextCall(op, label, ref r);
    public static void Label(string text) { NativeRequest r = default; Widget(102, text, ref r); }
    public static bool Button(string label) { NativeRequest r = default; return Widget(103, label, ref r) != 0; }
    public static bool Toggle(string label, ref bool value) { NativeRequest r = new() { Result = value ? 1 : 0 }; bool changed = Widget(104, label, ref r) != 0; value = r.Result != 0; return changed; }
    public static bool FloatField(string label, ref float value) { NativeRequest r = new() { Value = value }; bool changed = Widget(105, label, ref r) != 0; value = r.Value; return changed; }
    public static bool IntField(string label, ref int value) { NativeRequest r = new() { Result = value }; bool changed = Widget(106, label, ref r) != 0; value = r.Result; return changed; }
    public static bool Vector3Field(string label, ref Vector3 value) { NativeRequest r = new() { A = value }; bool changed = Widget(107, label, ref r) != 0; value = r.A; return changed; }
    public static bool Slider(string label, ref float value, float min, float max) { NativeRequest r = new() { Value = value, A = new(min, max, 0) }; bool changed = Widget(108, label, ref r) != 0; value = r.Value; return changed; }
    public static void Separator() { NativeRequest r = default; Engine.Call(109, ref r); }
    public static bool TextField(string label, ref string value) {
        NativeRequest r = default;
        bool changed=Engine.TextCall(115,System.Text.Json.JsonSerializer.Serialize(new { label, value }),ref r)!=0;
        if(changed) value=System.Runtime.InteropServices.Marshal.PtrToStringUTF8(r.Text) ?? "";
        return changed;
    }
}
