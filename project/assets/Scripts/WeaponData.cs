using System.Numerics;
using System.Reflection;
using System.Text.Json;

namespace Tartarus.Gameplay;

internal static class WeaponData
{
    static readonly JsonSerializerOptions Options=new(){IncludeFields=true};
    static float Num(JsonElement e,string name,float fallback)=>e.TryGetProperty(name,out var v) && v.ValueKind==JsonValueKind.Number?v.GetSingle():fallback;
    static bool Bool(JsonElement e,string name,bool fallback)=>e.TryGetProperty(name,out var v) && (v.ValueKind==JsonValueKind.True || v.ValueKind==JsonValueKind.False)?v.GetBoolean():fallback;
    static Vector3 Vec(JsonElement e,string name,Vector3 fallback) {
        if(!e.TryGetProperty(name,out var v) || v.ValueKind!=JsonValueKind.Array || v.GetArrayLength()!=3)return fallback;
        return new(v[0].GetSingle(),v[1].GetSingle(),v[2].GetSingle());
    }
    internal static WeaponDefinition Import(JsonElement g)
    {
        var w=new WeaponDefinition();
        if(g.ValueKind!=JsonValueKind.Object)return w;
        w.Magazine=Math.Max(1,(int)Num(g,"magazine",w.Magazine));w.RoundsPerMinute=Num(g,"rpm",w.RoundsPerMinute);
        w.AllowFullAuto=Bool(g,"allowFullAuto",w.AllowFullAuto);
        float burst=Num(g,"burstRounds",0);
        if(!float.IsFinite(burst) || burst<0 || burst>32 || burst!=MathF.Floor(burst))throw new ArgumentException("gameplay.burstRounds must be an integer from 0 to 32");
        w.BurstRounds=(int)burst;w.ReloadHoldSeconds=Num(g,"reloadHoldSeconds",w.ReloadHoldSeconds);
        w.RegripMin=Num(g,"regripMin",w.RegripMin);w.RegripMax=Num(g,"regripMax",w.RegripMax);
        w.ImpactImpulse=Math.Max(0,Num(g,"impactImpulse",w.ImpactImpulse));w.ImpactMaxSpeed=Math.Max(0,Num(g,"impactMaxSpeed",w.ImpactMaxSpeed));
        w.ZeroDistance=Math.Max(0,Num(g,"zeroDistance",w.ZeroDistance));w.BulletHoleRadius=Num(g,"bulletHoleRadius",w.BulletHoleRadius);
        w.Pellets=Math.Clamp((int)Num(g,"pellets",w.Pellets),1,64);
        if(g.TryGetProperty("sightLine",out var sight)) {
            var origin=Vec(sight,"origin",default);var direction=Vec(sight,"direction",default);
            if(float.IsFinite(origin.LengthSquared()) && float.IsFinite(direction.LengthSquared()) && direction.Length()>1e-6f) {
                w.HasSightLine=true;w.SightOrigin=origin;w.SightDirection=Vector3.Normalize(direction);
            }
        }
        if(g.TryGetProperty("damage",out var damage)) {
            w.Damage=Num(damage,"base",w.Damage);w.HeadMultiplier=Num(damage,"head",w.HeadMultiplier);w.LimbMultiplier=Num(damage,"limb",w.LimbMultiplier);
            w.FalloffStart=Num(damage,"falloffStart",w.FalloffStart);w.FalloffEnd=Num(damage,"falloffEnd",w.FalloffEnd);w.FalloffMin=Num(damage,"falloffMin",w.FalloffMin);
        }
        if(g.TryGetProperty("spread",out var spread)) {w.SpreadHip=Num(spread,"hip",0);w.SpreadAds=Num(spread,"ads",0);}
        if(g.TryGetProperty("reload",out var reload) && reload.ValueKind==JsonValueKind.String && reload.GetString() is string mode && mode!="")
            w.Reload=mode=="magazine"?WeaponDefinition.ReloadStyle.Magazine:mode=="perRound"?WeaponDefinition.ReloadStyle.PerRound:throw new ArgumentException("'gameplay.reload' must be \"magazine\" or \"perRound\"");
        if(g.TryGetProperty("cycle",out var cycle)) {w.CycleAfterShot=Bool(cycle,"enabled",false);w.CycleDelay=Num(cycle,"delay",.1f);}
        Normalize(w);return w;
    }
    static float Bound(float v,float fallback,float low,float high)=>Math.Clamp(float.IsFinite(v)?v:fallback,low,high);
    static void Normalize(WeaponDefinition w)
    {
        if(!(w.RoundsPerMinute>0) || !float.IsFinite(w.RoundsPerMinute))throw new ArgumentException("'gameplay.rpm' must be a positive number");
        if(!(w.ReloadHoldSeconds>0))throw new ArgumentException("'gameplay.reloadHoldSeconds' must be positive");
        if(w.BurstRounds<0 || w.BurstRounds>32)throw new ArgumentException("gameplay.burstRounds must be an integer from 0 to 32");
        w.Magazine=Math.Max(1,w.Magazine);w.Pellets=Math.Clamp(w.Pellets,1,64);
        w.BulletHoleRadius=Bound(w.BulletHoleRadius,.004f,0,.1f);w.CycleDelay=Bound(w.CycleDelay,.1f,0,2);
        w.SpreadHip=Bound(w.SpreadHip,0,0,45);w.SpreadAds=Bound(w.SpreadAds,0,0,45);
        w.Damage=Bound(w.Damage,34,0,10000);w.HeadMultiplier=Bound(w.HeadMultiplier,3,0,100);w.LimbMultiplier=Bound(w.LimbMultiplier,.75f,0,100);
        w.FalloffStart=Bound(w.FalloffStart,25,0,10000);w.FalloffEnd=Math.Max(w.FalloffStart,Bound(w.FalloffEnd,60,0,10000));w.FalloffMin=Bound(w.FalloffMin,.6f,0,1);
        if(w.RegripMax<w.RegripMin)(w.RegripMin,w.RegripMax)=(w.RegripMax,w.RegripMin);
    }
    static string Save(WeaponDefinition w)=>JsonSerializer.Serialize(typeof(WeaponDefinition).GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.DeclaredOnly).ToDictionary(f=>f.Name,f=>f.GetValue(w)),Options);
    internal static string Request(string operation,string json)
    {
        using var document=JsonDocument.Parse(json);
        if(operation=="weapon.import")return Save(Import(document.RootElement));
        var w=JsonSerializer.Deserialize<WeaponDefinition>(json,Options)??new();Normalize(w);return Save(w);
    }
}
