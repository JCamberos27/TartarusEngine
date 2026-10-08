using System.Reflection;
using System.Text.Json;
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
        bool Awoken = false, bool Enabled = false, bool Started = false, bool PendingReload=false);
    static Context? context;
    static Assembly? assembly;
    static IProjectIntegration? integration;
    static readonly Dictionary<ulong, Instance> instances = new();
    static readonly Dictionary<string, string> descriptions = new();
    static string assemblyPath = "";
    static readonly Dictionary<string,string> operationNames=new(StringComparer.Ordinal);
    static string OperationName(nint address){
        if(address==0)return "";byte* bytes=(byte*)address;int count=0;while(count<256 && bytes[count]!=0)count++;
        if(count==256)return Utf8(address);
        Span<char> chars=stackalloc char[256];int length=System.Text.Encoding.UTF8.GetChars(new ReadOnlySpan<byte>(bytes,count),chars);var name=chars[..length];
        if(operationNames.GetAlternateLookup<ReadOnlySpan<char>>().TryGetValue(name,out var existing))return existing;
        string value=new(name);if(operationNames.Count<256)operationNames[value]=value;return value;
    }
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
        Context? nextContext=string.IsNullOrEmpty(path)?null:new Context(path);
        try
        {
            Assembly next=nextContext==null?typeof(Script).Assembly:nextContext.LoadFromStream(new MemoryStream(File.ReadAllBytes(path)));
            Type? rootType=next.GetTypes().SingleOrDefault(t=>!t.IsAbstract && typeof(IProjectIntegration).IsAssignableFrom(t));
            IProjectIntegration? nextIntegration=rootType==null?null:(IProjectIntegration)Activator.CreateInstance(rootType)!;
            if(integration!=null && nextIntegration!=null)nextIntegration.LoadState(integration.SaveState());
            // Construct and validate replacements before touching the running assembly.
            var replacements = new Dictionary<ulong, Instance>();
            foreach (var (entity, item) in instances)
            {
                Script script = Make(next, (uint)entity, (uint)(entity >> 32), item.Class, item.Fields);
                script.IsEnabled = item.Script.IsEnabled;
                replacements.Add(entity, item with { Script = script, Faulted = false, PendingReload=item.Awoken });
            }
            var states = instances.ToDictionary(x => x.Key, x => x.Value.Script.SaveState());
            foreach (var (entity, item) in replacements) item.Script.LoadState(states[entity]);
            Context? previous = context;
            context = nextContext; assembly = next; integration = nextIntegration; assemblyPath = path;
            instances.Clear(); foreach (var pair in replacements) instances.Add(pair.Key, pair.Value);
            descriptions.Clear();
            previous?.Unload();
            Engine.Log(string.IsNullOrEmpty(path)?"C# host ready (no project assembly)":"C# gameplay loaded: "+Path.GetFileName(path));
        }
        catch { nextContext?.Unload(); throw; }
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
                case 3:
                    if (size != sizeof(EntityFrame) || assembly == null) return -2;
                    Entity(ref *(EntityFrame*)data); return 0;
                case 4:
                    foreach (var item in instances.Values.ToArray()) Destroy(item);
                    instances.Clear(); return 0;
                case 5: Reload(assemblyPath); return 0;
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
                case 13:
                    if(size!=sizeof(NativeRequest) || assembly==null)return -2;
                    // Resolve defaults and serialized overrides without constructing a live scene instance.
                    var request=(NativeRequest*)data;
                    using(var fields=System.Text.Json.JsonDocument.Parse(Utf8(request->Text))) {
                        var root=fields.RootElement;
                        var script=Make(assembly,uint.MaxValue,0,root.GetProperty("class").GetString()!,root.GetProperty("fields").GetRawText());
                        Engine.TextCall(30,ScriptFields.Save(script),ref *request);
                    }
                    return 0;
                case 14:
                    if(size!=sizeof(ProjectCall) || integration == null)return -2;
                    var call=(ProjectCall*)data;
                    return integration.Invoke(OperationName(call->Operation),call->Data,call->Size)?0:-2;
                case 15:
                    if(size!=sizeof(NativeRequest) || integration == null)return -2;
                    var jsonRequest=(NativeRequest*)data;
                    using(var document=System.Text.Json.JsonDocument.Parse(Utf8(jsonRequest->Text))) {
                        var root=document.RootElement;
                        Engine.TextCall(30,integration.Request(root.GetProperty("operation").GetString()!,root.GetProperty("data").GetRawText()),ref *jsonRequest);
                    }
                    return 0;
                case 16:
                    if(size!=sizeof(NativeRequest))return -2;
                    DeliverPhysics(Utf8(((NativeRequest*)data)->Text));return 0;
                case 17:
                    if (size != sizeof(NativeRequest) || assembly == null) return -2;
                    InvokeEditorMethod(ref *(NativeRequest*)data); return 0;
                case 18:
                    if (size != sizeof(NativeRequest) || assembly == null) return -2;
                    ShowValues(ref *(NativeRequest*)data); return 0;
                default: return -2;
            }
        }
        catch (Exception e) { Report(e); return -1; }
    }
    static void DeliverPhysics(string json)
    {
        using var document=System.Text.Json.JsonDocument.Parse(json);
        var receivers=instances.ToArray().ToLookup(pair=>(uint)pair.Key);
        foreach(var e in document.RootElement.EnumerateArray()) {
            uint target=e.GetProperty("target").GetUInt32(),other=e.GetProperty("other").GetUInt32();
            int phase=e.GetProperty("phase").GetInt32();bool trigger=e.GetProperty("trigger").GetBoolean();
            foreach(var pair in receivers[target]) {
                var item=pair.Value;
                if((uint)pair.Key!=target || !item.Enabled || item.Faulted || !item.Script.IsEnabled)continue;
                if(!item.Script.gameObject.IsValid || !item.Script.gameObject.activeInHierarchy)continue;
                try {
                    if(trigger) {
                        if(phase==0)item.Script.OnTriggerEnter(new(other));
                        else if(phase==1)item.Script.OnTriggerStay(new(other));else item.Script.OnTriggerExit(new(other));
                    } else {
                        var point=e.GetProperty("point").Deserialize<System.Numerics.Vector3>(NativeServices.Json);
                        var normal=e.GetProperty("normal").Deserialize<System.Numerics.Vector3>(NativeServices.Json);
                        var collision=new Collision(new(other),point,normal,e.GetProperty("impulse").GetSingle(),e.GetProperty("speed").GetSingle());
                        if(phase==0)item.Script.OnCollisionEnter(collision);
                        else if(phase==1)item.Script.OnCollisionStay(collision);else item.Script.OnCollisionExit(collision);
                    }
                } catch(Exception error) {instances[pair.Key]=item with {Faulted=true};Report(error);}
            }
        }
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
            if(item.PendingReload) {item=item with {PendingReload=false};instances[entity]=item;item.Script.OnReload();}
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
    // vInspector (ops 13 / 14). The request text is {"class","fields","method"}; Entity / Script
    // name the live instance, used while Playing. Otherwise a temporary instance holds the saved
    // fields, so edit-mode buttons and read-outs never touch a running behaviour.
    static Script Target(System.Text.Json.JsonElement root, uint entity, uint slot)
    {
        string name = root.GetProperty("class").GetString() ?? "";
        if (instances.TryGetValue(Key(entity, slot), out var live) && live.Class == name) return live.Script;
        return Make(assembly!, entity, slot, name, root.TryGetProperty("fields", out var f) ? f.GetString() ?? "{}" : "{}");
    }
    // For EditorHost (custom inspectors): the same as ops 13 / 14, from managed code.
    internal static string? RunScriptMethod(string className, string fields, uint entity, uint slot, string method, out bool ran)
    {
        ran = false;
        if (assembly == null) return null;
        Script s = instances.TryGetValue(Key(entity, slot), out var live) && live.Class == className ? live.Script
                 : Make(assembly, entity, slot, className, fields);
        ran = ScriptFields.Invoke(s, method);
        return ScriptFields.Save(s);
    }
    internal static Dictionary<string, string> ScriptShowValues(string className, string fields, uint entity, uint slot)
    {
        if (assembly == null) return new();
        Script s = instances.TryGetValue(Key(entity, slot), out var live) && live.Class == className ? live.Script
                 : Make(assembly, entity, slot, className, fields);
        return System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, string>>(ScriptFields.ShowValues(s)) ?? new();
    }
    static void InvokeEditorMethod(ref NativeRequest request)
    {
        using var doc = System.Text.Json.JsonDocument.Parse(Utf8(request.Text));
        Script script = Target(doc.RootElement, request.Entity, request.Script);
        bool ran = ScriptFields.Invoke(script, doc.RootElement.GetProperty("method").GetString() ?? "");
        request.Result = ran ? 1 : 0;
        Engine.TextCall(30, ScriptFields.Save(script), ref request);
    }
    static void ShowValues(ref NativeRequest request)
    {
        using var doc = System.Text.Json.JsonDocument.Parse(Utf8(request.Text));
        Engine.TextCall(30, ScriptFields.ShowValues(Target(doc.RootElement, request.Entity, request.Script)), ref request);
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
