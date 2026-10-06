using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Tartarus.Editor;

internal static class EditorHost
{
    static Entry.Context? context;
    static readonly Dictionary<Type, EditorWindow> windows = new();
    static MethodInfo[] menus = [];
    static Dictionary<string, Editor> inspectors = new();
    static readonly HashSet<string> faulted = new();
    static uint previousSelection=uint.MaxValue;
    static bool previousPlay;
    internal static bool ToolsVisible;
    static void Report(Exception e) => Engine.Log("C# error: " + (e.InnerException ?? e));
    internal static T GetWindow<T>() where T : EditorWindow
    {
        Engine.CheckThread();
        if (!windows.TryGetValue(typeof(T), out var window)) throw new InvalidOperationException("Window type is not in the editor assembly");
        window.Open = true; return (T)window;
    }
    internal static void Load(string path)
    {
        var nextContext = new Entry.Context(path);
        try {
            var assembly = nextContext.LoadFromStream(new MemoryStream(File.ReadAllBytes(path)));
            var next = new Dictionary<Type, EditorWindow>();
            foreach (var type in assembly.GetTypes().Where(t => !t.IsAbstract && typeof(EditorWindow).IsAssignableFrom(t))) {
                var window = (EditorWindow)Activator.CreateInstance(type)!;
                var old = windows.Values.FirstOrDefault(w => w.GetType().FullName == type.FullName);
                if (old != null) { window.LoadState(old.SaveState()); window.Open = old.Open; }
                next.Add(type, window);
            }
            var nextMenus = assembly.GetTypes().SelectMany(t => t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
                .Where(m => m.IsDefined(typeof(MenuItemAttribute))).OrderBy(m => m.GetCustomAttribute<MenuItemAttribute>()!.Path).ToArray();
            if (nextMenus.Any(m => m.GetParameters().Length != 0 || m.ReturnType != typeof(void))) throw new InvalidOperationException("MenuItem must be a static void method without parameters");
            var nextInspectors=new Dictionary<string,Editor>();
            foreach(var type in assembly.GetTypes().Where(t=>!t.IsAbstract && typeof(Editor).IsAssignableFrom(t)))
                foreach(var attribute in type.GetCustomAttributes<CustomEditorAttribute>())
                    nextInspectors.Add(attribute.Target,(Editor)Activator.CreateInstance(type)!);
            var oldContext = context;
            foreach (var window in windows.Values) try { window.OnDisable(); } catch (Exception e) { Report(e); }
            windows.Clear(); foreach (var pair in next) windows.Add(pair.Key, pair.Value);
            menus = nextMenus; inspectors=nextInspectors; context = nextContext; faulted.Clear(); oldContext?.Unload();
            foreach (var window in windows.Values) try { window.OnEnable(); } catch (Exception e) { faulted.Add(window.GetType().FullName!); Report(e); }
            Engine.Log("C# editor tools loaded: " + Path.GetFileName(path));
        } catch { nextContext.Unload(); throw; }
    }
    internal static void Stop()
    {
        ToolsVisible=false;
        foreach (var window in windows.Values) try { window.OnDisable(); } catch (Exception e) { Report(e); }
        windows.Clear(); menus = []; inspectors.Clear(); faulted.Clear(); context?.Unload(); context = null;
    }
    internal static void Draw(float dt)
    {
        if (context == null) return;
        NativeRequest r = default;
        bool visible;
        if(ToolsVisible) {
            r = new() { Result = 1 };
            visible = Engine.TextCall(100, "C# Tools###TartarusCSharpTools", ref r) != 0;
            ToolsVisible=r.Result!=0;
            try {
            if (visible) {
                foreach (var window in windows.Values) {
                    if (EditorGUILayout.Button("Open " + window.Title + "##" + window.GetType().FullName)) window.Open = true;
                    if (faulted.Contains(window.GetType().FullName!)) EditorGUILayout.Label("Tool failed. Check Console; save its source to retry.");
                }
                foreach (var menu in menus) {
                    string path = menu.GetCustomAttribute<MenuItemAttribute>()!.Path;
                    if (EditorGUILayout.Button(path)) try { menu.Invoke(null, null); } catch (Exception e) { Report(e); }
                }
            }
            } finally { Engine.Call(101, ref r); }
        }
        uint selection=Selection.activeGameObject?.Id ?? uint.MaxValue;
        bool playing=Application.isPlaying;
        foreach (var window in windows.Values.ToArray()) {
            string id = window.GetType().FullName!;
            if (faulted.Contains(id)) continue;
            try {
                if(selection!=previousSelection) window.OnSelectionChanged();
                if(playing!=previousPlay) window.OnPlayModeChanged(playing);
                window.Update(dt);
            } catch (Exception e) { faulted.Add(id); Report(e); continue; }
            if (!window.Open) continue;
            r = new() { Result = 1 };
            visible = Engine.TextCall(100, window.Title + "###csharp:" + id, ref r) != 0;
            window.Open = r.Result != 0;
            try { if (visible) window.OnGUI(); } catch (Exception e) { faulted.Add(id); Report(e); }
            finally { Engine.Call(101, ref r); }
        }
        previousSelection=selection;previousPlay=playing;
    }
    internal static void DrawInspector(ref NativeRequest frame)
    {
        frame.Result=0;
        using var document=JsonDocument.Parse(Marshal.PtrToStringUTF8(frame.Text)!);
        var data=document.RootElement;
        string name=data.GetProperty("type").GetString()!;
        if(!inspectors.TryGetValue(name,out var editor) && (data.GetProperty("native").GetBoolean() || !inspectors.TryGetValue("*",out editor))) return;
        string id="inspector:"+editor.GetType().FullName;
        if(faulted.Contains(id)) return;
        try {
            editor.target=new(frame.Entity);
            editor.serializedObject=new(frame.Entity,data);
            editor.OnInspectorGUI();
            string values=editor.serializedObject.ToJson();
            Engine.TextCall(116,values,ref frame);frame.Result=1;
        } catch(Exception e) {faulted.Add(id);Report(e);}
        finally {editor.serializedObject=null!;editor.target=GameObject.Invalid;}
    }
}
