using System.Text.Json;
using Tartarus;
namespace Tartarus.Gameplay;

/// <summary>Game health. This script owns the state; native presentation reads projections.</summary>
public sealed class Health : Script
{
    [Range(1,100000)] public float Max=100;
    public bool Invulnerable;
    public float Current=>HealthRules.Find(Entity)?.Current??Max;
    public float ApplyDamage(float amount)=>HealthRules.Damage(Entity,amount);
    public void Restore()=>HealthRules.Initialise(Entity,Max,Invulnerable,true);
    public override void Awake()=>HealthRules.Initialise(Entity,Max,Invulnerable,false);
    public override void OnEnable()=>HealthRules.Enable(Entity,true);
    public override void OnDisable()=>HealthRules.Enable(Entity,false);
    public override void OnReload()=>HealthRules.Initialise(Entity,Max,Invulnerable,false);
    public override void Update()=>HealthRules.Configure(Entity,Max,Invulnerable);
    public override void OnDestroy()=>HealthRules.Remove(Entity);
}

internal static class HealthRules
{
    internal sealed class State {public float Max,Current;public bool Invulnerable,Enabled=true;}
    static Dictionary<uint,State> states=new();
    static readonly JsonSerializerOptions Json=new() {IncludeFields=true};
    internal static State? Find(uint entity)=>states.GetValueOrDefault(entity);
    internal static string Save()=>JsonSerializer.Serialize(states,Json);
    internal static void Load(string json)=>states=JsonSerializer.Deserialize<Dictionary<uint,State>>(json,Json)??new();
    internal static void Remove(uint entity)=>states.Remove(entity);
    internal static void Enable(uint entity,bool enabled){if(Find(entity) is {} s)s.Enabled=enabled;}
    internal static void Configure(uint entity,float maximum,bool invulnerable) {
        if(Find(entity) is {} s){s.Max=Math.Clamp(maximum,1,100000);s.Current=Math.Min(s.Current,s.Max);s.Invulnerable=invulnerable;}
    }
    internal static State Initialise(uint entity,float maximum,bool invulnerable,bool reset) {
        maximum=Math.Clamp(maximum,1,100000);
        if(!states.TryGetValue(entity,out var s)){s=new() {Max=maximum,Current=maximum,Invulnerable=invulnerable};states.Add(entity,s);}
        else {s.Max=maximum;s.Current=reset?maximum:Math.Min(s.Current,maximum);s.Invulnerable=invulnerable;}
        if(reset)s.Enabled=true;return s;
    }
    internal static float Damage(uint entity,float amount) {
        var s=Find(entity);if(s==null || !s.Enabled || s.Invulnerable || amount<=0)return 0;
        float taken=Math.Min(s.Current,amount);s.Current=Math.Max(0,s.Current-amount);return taken;
    }
    internal static string Request(string operation,string json) {
        using var doc=JsonDocument.Parse(json);var r=doc.RootElement;uint id=r.GetProperty("entity").GetUInt32();
        if(operation=="health.remove"){Remove(id);return "{}";}
        if(operation=="health.kill"){if(Find(id) is {} s)s.Current=0;return "{}";}
        if(operation=="health.create") {
            var fields=r.GetProperty("fields");float max=fields.TryGetProperty("Max",out var m)?m.GetSingle():new Health().Max;
            bool inv=fields.TryGetProperty("Invulnerable",out var i) && i.GetBoolean();Initialise(id,max,inv,true);
        }
        return JsonSerializer.Serialize(Find(id),Json);
    }
}
