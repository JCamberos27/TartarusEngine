using System.Text.Json;
namespace Tartarus.Gameplay;
internal static class ImpactPolicy
{
    internal static void Update(ref ImpactFrame f){
        switch(f.Operation){
        case 0:f.Result=f.Enabled && f.CasingsEnabled && f.Contact>=0 && f.Contact<f.MaxContacts && f.Speed>=f.MinSpeed?1:0;break;
        case 1:float k=Math.Clamp((f.Speed-f.MinSpeed)/Math.Max(f.FullSpeed-f.MinSpeed,1e-3f),0,1);f.Gain=f.Volume*(f.GainMin+(1-f.GainMin)*k);break;
        case 2:f.Result=f.Radius>f.ShellRadius?1:0;break;
        case 3:f.Gain=f.Miss>f.FlybyRadius || f.FlybyRadius<=1e-4f?0:f.FlybyVolume*(1+(f.FarGain-1)*Math.Clamp(f.Miss/f.FlybyRadius,0,1));break;
        case 4:f.Result=f.Now-f.Last<f.Interval?0:1;if(f.Result!=0)f.Last=f.Now;break;
        }
    }
    internal static string Request(string operation,string json){
        if(operation=="audio.cue"){
            int cue=JsonSerializer.Deserialize<int>(json);
            return cue switch {
                0 or 1 or 2=>JsonSerializer.Serialize(new {kind=0}),
                4=>JsonSerializer.Serialize(new {kind=1}),
                3=>JsonSerializer.Serialize(new {kind=2,key="snd.body_fall",at2d=false,set=new {minDistance=2,maxDistance=30,maxVoices=4,pitchMin=.95f,pitchMax=1.05f,volumeJitterDb=1}}),
                5 or 6=>JsonSerializer.Serialize(new {kind=2,key=cue==6?"snd.ui.hitmarker_kill":"snd.ui.hitmarker",at2d=true,set=new {maxVoices=4,stealFadeTime=.01f,reverbSend=0,occlusion=false}}),
                _=>throw new ArgumentOutOfRangeException(nameof(cue))
            };
        }
        using var doc=JsonDocument.Parse(json);var x=doc.RootElement;
        if(operation=="audio.impact-surface"){string surface=x.GetProperty("surface").GetString()!;return JsonSerializer.Serialize(x.GetProperty("available").EnumerateArray().Any(v=>v.GetString()==surface)?surface:x.GetProperty("fallback").GetString());}
        string kind=x.GetProperty("kind").GetString()!,category=x.GetProperty("surface").GetString()!;
        string key=kind switch {"impact"=>"snd.impact."+category,"casing"=>"snd.casing."+(x.GetProperty("shell").GetBoolean()?"shell.":"rifle.")+category,"flyby"=>"snd.flyby",_=>throw new ArgumentException(kind)};
        return JsonSerializer.Serialize(new {key,set=new {minDistance=x.GetProperty("min").GetSingle(),maxDistance=x.GetProperty("max").GetSingle(),maxVoices=Math.Max(1,x.GetProperty("voices").GetInt32()),stealFadeTime=kind=="casing"?.05f:.03f,pitchMin=kind=="casing"?.96f:kind=="impact"?.97f:.95f,pitchMax=kind=="casing"?1.04f:kind=="impact"?1.03f:1.05f,volumeJitterDb=1}});
    }
}
