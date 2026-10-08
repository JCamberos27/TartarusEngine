using Tartarus;
using System.Text.Json.Nodes;

namespace Tartarus.Gameplay;

public sealed class ImpactSound : Script
{
    [AssetPath(".wav",".ogg",".mp3")] public AssetReference Clip=new();
    [Range(0,1)] public float Volume=1;
    [Range(0,100)] public float MinSpeed=.6f;
    [Range(.01f,200)] public float MaxSpeed=8;
    [Range(0,1)] public float PitchVariation=.08f;
    double last=-1;
    public override string SaveState() {var state=JsonNode.Parse(base.SaveState())!;state["_last"]=last;return state.ToJsonString();}
    public override void LoadState(string json) {base.LoadState(json);var state=JsonNode.Parse(json)!;if(state["_last"] is { } value)last=value.GetValue<double>();}
    static ulong step=ulong.MaxValue;
    static int played;
    public override void OnCollisionEnter(Collision collision) {
        BasketState.Track();ulong sequence=Physics.stepSequence;if(step!=sequence) {step=sequence;played=0;}
        if(played>=12 || collision.NormalSpeed<=0 || collision.NormalSpeed<MinSpeed || string.IsNullOrEmpty(Clip.Path) || (last>=0 && BasketState.Now-last<.06))return;
        last=BasketState.Now;
        float t=Math.Clamp((collision.NormalSpeed-MinSpeed)/Math.Max(MaxSpeed-MinSpeed,.01f),0,1);
        Audio.PlayAtPosition(Clip.ResolvePath(),collision.Point,Volume*(.12f+.88f*MathF.Sqrt(t)),pitch:1+(Random.Shared.NextSingle()*2-1)*PitchVariation);played++;
    }
}
