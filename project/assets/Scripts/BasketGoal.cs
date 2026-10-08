using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using Tartarus;

namespace Tartarus.Gameplay;

internal static class BasketState
{
    internal static readonly Dictionary<uint,Vector3> Releases=[];
    static uint held=uint.MaxValue;
    static ulong sequence=ulong.MaxValue;
    internal static double Now;
    internal static void Track() {
        ulong step=Physics.stepSequence;if(sequence==step)return;sequence=step;Now+=Time.deltaTime;
        var next=Physics.constrainedObject;uint id=next?.Id??uint.MaxValue;
        if(held!=uint.MaxValue && id!=held && Physics.TryGetBodyPosition(new(held),out var point)) Releases[held]=point;
        if(id!=uint.MaxValue) Releases.Remove(id);held=id;
    }
    internal static void Reset() { Releases.Clear();held=uint.MaxValue;sequence=ulong.MaxValue;Now=0; }
    internal static string Save() => JsonSerializer.Serialize(new Snapshot(Now,held,sequence,Releases),new JsonSerializerOptions {IncludeFields=true});
    internal static void Restore(string json) {
        var snapshot=JsonSerializer.Deserialize<Snapshot>(json,new JsonSerializerOptions {IncludeFields=true})!;
        Now=snapshot.Now;held=snapshot.Held;sequence=snapshot.Sequence;Releases.Clear();foreach(var pair in snapshot.Releases)Releases.Add(pair.Key,pair.Value);
    }
    sealed record Snapshot(double Now,uint Held,ulong Sequence,Dictionary<uint,Vector3> Releases);
}

public sealed class BasketGoal : Script
{
    public string Tag="Ball";
    public ScoreTeam Team;
    [Range(0,100)] public int Points=2;
    [Range(0,100)] public int ThreePoints=3;
    [Range(0,1000)] public float ThreePointDistance;
    public bool RequireDownward=true;
    [AssetPath(".wav",".ogg",".mp3")] public AssetReference ScoreSound=new();
    [Range(0,1000)] public float FlashIntensity=40;
    Dictionary<uint,double> cooldown=[];
    Dictionary<uint,float> baselines=[];
    readonly List<(GameObject Object,float Base)> lights=[];
    readonly List<GameObject> particles=[];
    [SerializeField,HideInInspector] float burst,boost;
    public override void OnReload() => Start();
    public override string SaveState() {
        var state=JsonNode.Parse(base.SaveState())!;
        state["_cooldown"]=JsonSerializer.SerializeToNode(cooldown);state["_baselines"]=JsonSerializer.SerializeToNode(baselines);state["_basket"]=BasketState.Save();return state.ToJsonString();
    }
    public override void LoadState(string json) {
        base.LoadState(json);var state=JsonNode.Parse(json)!;
        if(state["_cooldown"] is { } c)cooldown=c.Deserialize<Dictionary<uint,double>>()!;
        if(state["_baselines"] is { } b)baselines=b.Deserialize<Dictionary<uint,float>>()!;
        if(state["_basket"] is { } basket)BasketState.Restore(basket.GetValue<string>());
    }
    public override void Start() {
        foreach(var c in gameObject.children) {
            if(c.GetComponent<Light>() is { } light) {if(!baselines.TryGetValue(c.Id,out float baseline))baselines[c.Id]=baseline=light.intensity;lights.Add((c,baseline));}
            if(c.HasNativeComponent("Particle System")) particles.Add(c);
        }
    }
    public override void OnTriggerEnter(GameObject other) {
        BasketState.Track();if(!other.IsValid || other.tag!=Tag)return;
        if(RequireDownward && (other.GetComponent<Rigidbody>() is not { } body || body.velocity.Y>-.2f))return;
        if(cooldown.TryGetValue(other.Id,out double last) && BasketState.Now-last<1)return;
        cooldown[other.Id]=BasketState.Now;
        int points=Points;Vector3 goal=transform.position;
        if(Physics.TryGetBodyPosition(gameObject,out var p))goal=p;
        if(ThreePointDistance>0 && BasketState.Releases.TryGetValue(other.Id,out var release) && Vector2.Distance(new(release.X,release.Z),new(goal.X,goal.Z))>=ThreePointDistance)points=ThreePoints;
        BasketState.Releases.Remove(other.Id);Scoreboard.Current?.Add(Team,points);
        foreach(var particle in particles) if(particle.IsValid)particle.GetNativeComponent("Particle System").Set("Emitting",true);
        burst=.3f;boost=1;
        var sound=ScoreSound.ResolvePath();
        if(!string.IsNullOrEmpty(sound)) Audio.PlayAtPosition(sound,goal,points>=ThreePoints?1:.85f);
    }
    public override void Update() {
        BasketState.Track();float dt=Time.deltaTime;
        if(burst>0 && (burst-=dt)<=0) foreach(var particle in particles) if(particle.IsValid)particle.GetNativeComponent("Particle System").Set("Emitting",false);
        if(boost>0) {boost=Math.Max(0,boost-dt/1.2f);foreach(var (obj,baseline) in lights) if(obj.IsValid && obj.GetComponent<Light>() is { } light) light.intensity=baseline+(Math.Max(FlashIntensity,baseline)-baseline)*boost*boost;}
    }
    public override void OnDisable() {
        foreach(var particle in particles) if(particle.IsValid)particle.GetNativeComponent("Particle System").Set("Emitting",false);
        foreach(var (obj,baseline) in lights) if(obj.IsValid && obj.GetComponent<Light>() is { } light)light.intensity=baseline;
        burst=boost=0;
    }
}
