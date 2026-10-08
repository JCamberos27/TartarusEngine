using System.Numerics;
namespace Tartarus.Gameplay;
internal static unsafe class NpcCallouts
{
    readonly record struct Rule(int Priority,float Cooldown,float Duration,bool Response);
    static readonly Rule[] Rules=[new(7,8,2.63f,true),new(5,6,2.15f,false),new(6,6,2.26f,false),new(5,8,1.80f,true),new(5,8,1.79f,true),new(4,8,1.67f,true),new(6,10,1.99f,true),new(5,10,2.18f,false),new(8,5,2.36f,true),new(9,4,2.17f,false),new(3,10,1.38f,false),new(4,12,1.90f,false),new(6,20,1.91f,false),new(3,12,1.69f,false),new(4,10,1.77f,true),new(3,10,1.90f,false),new(2,15,1.64f,false),new(1,25,1.96f,false),new(8,3,2.16f,false),new(0,0,1.12f,false)];
    static float Hash(int a,float b){float v=MathF.Sin(a*12.9898f+b*78.233f)*43758.5453f;return v-MathF.Floor(v);}
    static void Call(ref NpcCalloutsFrame f,int caller,int kind) {
        var members=(NpcCallMemberFrame*)f.Members;var channels=(NpcCallChannelFrame*)f.Channels;
        if((uint)caller>=(uint)f.MemberCount || (uint)kind>=Rules.Length)return;
        ref var n=ref members[caller];if(!n.Exists || n.Dummy)return;
        int squad=Math.Max(0,n.Squad);if(squad>=f.ChannelCount)return;
        ref var c=ref channels[squad];var rule=Rules[kind];
        if(f.Now-n.LastCallout<2.5f && rule.Priority<7 || rule.Cooldown>0 && f.Now-c.LastEvent[kind]<rule.Cooldown)return;
        if(f.Now<c.BusyUntil && rule.Priority<=c.OnAirPriority)return;
        c.RespPending=false;c.OnAirPriority=rule.Priority;c.BusyUntil=f.Now+rule.Duration+.18f;c.LastEvent[kind]=f.Now;
        if(rule.Response){
            bool anyone=false;for(int i=0;i<f.MemberCount;i++){ref var o=ref members[i];if(o.Exists && !o.Dead && o.Index!=n.Index && o.Squad==n.Squad && Vector3.Distance(o.Feet,n.Feet)<40){anyone=true;break;}}
            f.Random=unchecked(f.Random*1664525+1013904223);
            uint random=(uint)f.Random>>8;
            if(anyone && random/16777216f<.6f){c.RespPending=true;c.RespAt=c.BusyUntil+.3f;}
        }
        n.LastCallout=f.Now;
        for(int i=0;i<f.MemberCount;i++){ref var o=ref members[i];if(!o.Exists || o.Dead || i==caller || o.Squad!=n.Squad || o.Fire || Vector3.Distance(o.Feet,n.Feet)>18)continue;o.GlanceAt=n.Eye;o.GlanceUntil=f.Now+.7f+.6f*Hash(o.Index,f.Now);}
    }
    internal static void Update(ref NpcCalloutsFrame f) {
        if(f.Operation==0){Call(ref f,f.Caller,f.Kind);return;}
        var members=(NpcCallMemberFrame*)f.Members;var channels=(NpcCallChannelFrame*)f.Channels;
        for(int i=0;i<f.ChannelCount;i++){ref var c=ref channels[i];if(!c.RespPending || f.Now<c.RespAt)continue;c.RespPending=false;if(f.Now>=c.BusyUntil){c.OnAirPriority=Rules[19].Priority;c.BusyUntil=f.Now+Rules[19].Duration+.18f;c.LastEvent[19]=f.Now;}}
        if(f.Player.Dead && !f.PlayerWasDead){int who=-1;float best=1e9f;for(int i=0;i<f.MemberCount;i++){ref var n=ref members[i];if(!n.Exists || n.Dead || !n.Known)continue;float d=Vector3.Distance(n.Feet,f.Player.Feet);if(d<best){best=d;who=i;}}if(who>=0)Call(ref f,who,12);}
        f.PlayerWasDead=f.Player.Dead;
        for(int i=0;i<f.MemberCount;i++) {
            ref var n=ref members[i];if(!n.Exists || n.Dead)continue;
            bool reload=n.Reloading && n.Known;if(reload && !n.VcReloading)Call(ref f,i,10);n.VcReloading=reload;
            bool cover=n.Suppress && n.TriggerHeld;if(cover && !n.VcCovering)Call(ref f,i,11);n.VcCovering=cover;
            bool suspicious=!n.Known && n.Awareness>.3f;if(suspicious && !n.VcSuspicious)Call(ref f,i,14);n.VcSuspicious=suspicious;
            if(n.Behaviour==0 && !n.Known){if(n.NextChatter<=0)n.NextChatter=f.Now+10+25*Hash(n.Index,f.Now);else if(f.Now>=n.NextChatter){Call(ref f,i,17);n.NextChatter=f.Now+25+35*Hash(n.Index+7,f.Now);}}
        }
    }
}
