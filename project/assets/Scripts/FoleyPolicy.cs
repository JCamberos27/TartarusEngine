using System.Text.Json;
namespace Tartarus.Gameplay;
internal static class FoleyPolicy
{
    internal static string Surface(string json) {
        using var data=JsonDocument.Parse(json);var r=data.RootElement;
        string name=r.GetProperty("name").GetString()!.ToLowerInvariant();
        foreach(string entry in r.GetProperty("table").GetString()!.Split(';')) {
            int eq=entry.IndexOf('=');if(eq<=0)continue;
            foreach(string word in entry[(eq+1)..].Split(',')) {string match=word.Trim(' ').ToLowerInvariant();if(match.Length>0 && name.Contains(match))return JsonSerializer.Serialize(entry[..eq]);}
        }
        return JsonSerializer.Serialize(r.GetProperty("fallback").GetString());
    }
    internal static string Element(int id)=>id switch {0=>"walk",1=>"run",2=>"crouch",3=>"land",4=>"jump",10=>"land_light",11=>"land_heavy",_=>throw new ArgumentOutOfRangeException(nameof(id))};
    internal static string Request(string operation,string json){
        if(operation=="audio.foley-element")return JsonSerializer.Serialize(Element(JsonSerializer.Deserialize<int>(json)));
        using var doc=JsonDocument.Parse(json);var x=doc.RootElement;string surface=x.GetProperty("surface").GetString()!,element=x.GetProperty("element").GetString()!,fallback="step_"+x.GetProperty("fallback").GetString();
        var choices=new List<object>{new {surface,element,selectEmpty=true}};if(element=="crouch")choices.Add(new {surface,element="walk",selectEmpty=false});if(surface!=fallback && surface.StartsWith("step_",StringComparison.Ordinal))choices.Add(new {surface=fallback,element,selectEmpty=true});return JsonSerializer.Serialize(choices);
    }
    internal static void Update(ref FoleyFrame f) {
        if(f.Operation==0){f.Result=Math.Max(.05f,.5f*(f.Sprinting?f.SprintStride:f.WalkStride)*f.StepStrideScale*(f.Crouched?f.CrouchStrideScale:1));return;}
        if(f.Operation==1){f.Result=f.FallSpeed<f.LandMinSpeed?0:f.LandVolume*(.3f+.7f*Math.Clamp((f.FallSpeed-f.LandMinSpeed)/Math.Max(f.LandFullSpeed-f.LandMinSpeed,.001f),0,1));return;}
        if(f.Operation==2){f.Element=f.Crouched?2:f.Sprinting || f.Speed>=f.RunSpeed?1:0;f.Gain=f.Crouched?f.CrouchVolume:f.Element==1?f.RunVolume:f.WalkVolume;return;}
        if(f.Operation==3){f.Element=f.Sprinting?1:0;f.Gain=f.NpcStepVolume*(f.Sprinting?f.RunVolume:f.WalkVolume)/Math.Max(f.WalkVolume,.01f);return;}
        if(f.Operation==5){
            f.Landing=f.Grounded && !f.PrevGrounded;f.ResetSteps=f.Landing;f.UseFeet=f.StepsFromFeet && f.HaveFootHeights;
            f.ResetFeet=f.Landing || !f.Grounded || !f.UseFeet;f.UseFeet=f.Grounded && f.UseFeet;f.UseDistance=f.Grounded && !f.UseFeet && f.Speed>=f.MinStepSpeed;
            f.FallSpeed=Math.Max(0,-f.PrevVy);f.PrevGrounded=f.Grounded;f.PrevVy=f.Grounded?0:Math.Min(f.PrevVy,f.Vy);return;
        }
        if(f.Operation==7){f.Result=f.Speed>=f.MinStepSpeed?f.FootLiftMoving:f.FootLiftHeight;return;}
        if(f.Operation==4){f.Element=f.FallSpeed>=f.LandHeavySpeed?1:0;return;}
    }
}
