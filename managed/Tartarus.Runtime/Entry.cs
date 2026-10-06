using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Tartarus;

/// <summary>The only assembly pinned by hostfxr. Gameplay is loaded from streams so builds never lock it.</summary>
public static unsafe class Entry
{
    internal sealed class Context(string path) : AssemblyLoadContext(isCollectible: true)
    {
        readonly AssemblyDependencyResolver resolver = new(path);
        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == typeof(Script).Assembly.GetName().Name) return typeof(Script).Assembly;
            string? file = resolver.ResolveAssemblyToPath(name);
            return file == null ? null : LoadFromStream(new MemoryStream(File.ReadAllBytes(file)));
        }
    }
    sealed record Instance(Script Script, string Class, string Fields, bool Faulted = false,
        bool Awoken = false, bool Enabled = false, bool Started = false);
    static Context? context;
    static Assembly? assembly;
    static IGameplay? gameplay;
    static readonly Dictionary<ulong, Instance> instances = new();
    static readonly Dictionary<string, string> descriptions = new();
    static string assemblyPath = "";
    static string Utf8(nint text) => Marshal.PtrToStringUTF8(text) ?? "";
    static void Report(Exception e) { try { Engine.Log("C# error: " + (e.InnerException ?? e)); } catch { } }
    static ulong Key(uint entity, uint slot) => ((ulong)slot << 32) | entity;
    internal static IEnumerable<Script> GetScripts(uint entity)
    {
        Engine.CheckThread();
        return instances.Where(p => (uint)p.Key == entity).Select(p => p.Value.Script).ToArray();
    }
    static Script Make(Assembly source, uint entity, uint slot, string className, string fields)
    {
        Type t = source.GetType(className, throwOnError: true)!;
        if (!typeof(Script).IsAssignableFrom(t) || t.IsAbstract) throw new ArgumentException(className + " must derive from Tartarus.Script");
        var s = (Script)Activator.CreateInstance(t)!; s.Entity = entity; s.SlotId = slot;
        s.IsEnabled = false; ScriptFields.Apply(s, fields); return s;
    }
    static void Reload(string path)
    {
        var nextContext = new Context(path);
        try
        {
            Assembly next = nextContext.LoadFromStream(new MemoryStream(File.ReadAllBytes(path)));
            IGameplay nextGameplay = (IGameplay)Activator.CreateInstance(next.GetTypes().Single(t => !t.IsAbstract && typeof(IGameplay).IsAssignableFrom(t)))!;
            // Construct and validate replacements before touching the running assembly.
            var replacements = new Dictionary<ulong, Instance>();
            foreach (var (entity, item) in instances)
            {
                Script script = Make(next, (uint)entity, (uint)(entity >> 32), item.Class, item.Fields);
                script.IsEnabled = item.Script.IsEnabled;
                replacements.Add(entity, item with { Script = script, Faulted = false });
            }
            var states = instances.ToDictionary(x => x.Key, x => x.Value.Script.SaveState());
            foreach (var (entity, item) in replacements) item.Script.LoadState(states[entity]);
            Context? previous = context;
            context = nextContext; assembly = next; gameplay = nextGameplay; assemblyPath = path;
            instances.Clear(); foreach (var pair in replacements) instances.Add(pair.Key, pair.Value);
            descriptions.Clear();
            previous?.Unload();
            Engine.Log("C# gameplay loaded: " + Path.GetFileName(path));
        }
        catch { nextContext.Unload(); throw; }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Dispatch(int op, void* data, int size, void* callback)
    {
        Engine.Callback = (delegate* unmanaged[Cdecl]<int, NativeRequest*, int>)callback;
        Engine.ThreadId = Environment.CurrentManagedThreadId;
        try
        {
            switch (op)
            {
                case 0:
                    if (size != ScriptAbi.Version) return -2;
                    Reload(Utf8((nint)data)); return 0;
                case 1:
                    if (size != sizeof(PlayerFrame) || gameplay == null) return -2;
                    gameplay.Player(ref *(PlayerFrame*)data); return 0;
                case 2:
                    if (size != sizeof(WeaponFrame) || gameplay == null) return -2;
                    gameplay.Weapon(ref *(WeaponFrame*)data); return 0;
                case 3:
                    if (size != sizeof(EntityFrame) || assembly == null) return -2;
                    Entity(ref *(EntityFrame*)data); return 0;
                case 4:
                    foreach (var item in instances.Values.ToArray()) Destroy(item);
                    instances.Clear(); return 0;
                case 5: Reload(assemblyPath); return 0;
                case 6:
                    if (size != sizeof(ShotFrame) || gameplay == null) return -2;
                    gameplay.Shot(ref *(ShotFrame*)data); return 0;
                case 7:
                    if (size != sizeof(NativeRequest) || assembly == null) return -2;
                    Describe(ref *(NativeRequest*)data); return 0;
                case 8:
                    if (size != ScriptAbi.Version) return -2;
                    Editor.EditorHost.Load(Utf8((nint)data)); return 0;
                case 9:
                    if (size != sizeof(NativeRequest)) return -2;
                    Editor.EditorHost.Draw(((NativeRequest*)data)->Value); return 0;
                case 10: Editor.EditorHost.Stop(); return 0;
                case 11:
                    if(size!=sizeof(NativeRequest)) return -2;
                    Editor.EditorHost.DrawInspector(ref *(NativeRequest*)data);return 0;
                case 12:
                    if(size!=sizeof(NativeRequest))return -2;
                    if(((NativeRequest*)data)->Entity!=0)Editor.EditorHost.ToolsVisible=((NativeRequest*)data)->Result!=0;
                    ((NativeRequest*)data)->Result=Editor.EditorHost.ToolsVisible?1:0;return 0;
                default: return -2;
            }
        }
        catch (Exception e) { Report(e); return -1; }
    }
    static void Entity(ref EntityFrame frame)
    {
        Time.deltaTime = frame.Dt;
        if (frame.Phase == 2) Time.fixedDeltaTime = frame.Dt;
        ulong entity = Key(frame.Entity, frame.Script);
        if (frame.Phase == 3)
        {
            if (instances.Remove(entity, out var gone)) Destroy(gone);
            return;
        }
        string name = Utf8(frame.ClassName), fields = Utf8(frame.Fields);
        if (!instances.TryGetValue(entity, out var item) || item.Class != name)
        {
            if (item != null) { instances.Remove(entity); Destroy(item); }
            Script script = Make(assembly!, frame.Entity, frame.Script, name, fields);
            item = new(script, name, fields); instances.Add(entity, item);
        }
        else if (item.Fields != fields) { ScriptFields.ApplyChanges(item.Script, item.Fields, fields); item = item with { Fields = fields, Faulted = false }; instances[entity] = item; }
        item.Script.IsEnabled = frame.Enabled != 0;
        if (frame.Phase == 5) return; // all components are constructed before any Awake/Start
        if (item.Faulted) return;
        try
        {
            if (frame.Phase == 4) {
                if (item.Enabled) { item = item with { Enabled = false }; instances[entity] = item; item.Script.OnDisable(); }
                return;
            }
            if (!item.Awoken) { item = item with { Awoken = true }; instances[entity] = item; item.Script.Awake(); }
            if (!item.Enabled) { item = item with { Enabled = true }; instances[entity] = item; item.Script.OnEnable(); }
            if (frame.Phase == 0) return;
            if (!item.Started) { item = item with { Started = true }; instances[entity] = item; item.Script.Start(); }
            if (frame.Phase == 2) item.Script.FixedUpdate(frame.Dt);
            else if (frame.Phase == 1) item.Script.Update(frame.Dt);
            else if (frame.Phase == 6) item.Script.LateUpdate();
        }
        catch (Exception e) { instances[entity] = item with { Faulted = true }; Report(e); }
    }
    static void Destroy(Instance item)
    {
        if (item.Enabled) { item.Script.IsEnabled = false; try { item.Script.OnDisable(); } catch (Exception e) { Report(e); } }
        if (item.Awoken) { try { item.Script.OnDestroy(); } catch (Exception e) { Report(e); } }
    }
    static void Describe(ref NativeRequest request)
    {
        string name = Utf8(request.Text);
        if (!descriptions.TryGetValue(name, out string? json))
        {
            Type[] types = assembly!.GetTypes().Where(t => !t.IsAbstract && typeof(Script).IsAssignableFrom(t)).OrderBy(t => t.FullName).ToArray();
            json = name.Length == 0 ? System.Text.Json.JsonSerializer.Serialize(new { classes = types.Select(t => t.FullName).ToArray() })
                : ScriptFields.DescribeJson(types.Single(t => t.FullName == name));
            descriptions[name] = json;
        }
        Engine.TextCall(30, json, ref request);
    }
}
