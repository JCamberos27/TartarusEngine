using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Tartarus;

namespace Tartarus.Gameplay;

// Layout, labels, feedback timing and feed state belong to the sample game.
internal static unsafe class CombatHud
{
    internal sealed class Row { public string Name=""; public bool Head; public float At; }
    internal sealed class State {
        public List<Row> Feed=new(); public int Kills,Streak;
        public float LastKillAt=-1e9f,StreakAt=-1e9f;
    }
    static State state=new();
    static readonly JsonSerializerOptions Json=new() {IncludeFields=true};
    internal static string Save()=>JsonSerializer.Serialize(state,Json);
    internal static void Load(string json)=>state=JsonSerializer.Deserialize<State>(json,Json)??new();
    static string String(nint p)=>p==0?"":Marshal.PtrToStringUTF8(p)??"";
    static float Clamp(float v)=>Math.Clamp(v,0,1);
    static Vector4 Color(float r,float g,float b,float a=1)=>new(r,g,b,a);
    internal static void Update(ref CombatHudFrame f) {
        if(f.Operation==0) state=new();
        else if(f.Operation==1) {
            state.Feed.Add(new() {Name=f.Unit>=0?"UNIT-"+f.Unit:"SOLDIER",Head=f.Head,At=f.Now});
            if(state.Feed.Count>5)state.Feed.RemoveAt(0);
            state.Kills++;state.Streak=f.Now-state.LastKillAt<=f.StreakWindow?state.Streak+1:1;
            state.LastKillAt=f.Now;if(state.Streak>=2)state.StreakAt=f.Now;
        } else if(f.Operation==3) f.Result=f.Now<=f.StreakWindow?f.Streak+1:1;
        else if(f.Operation==4) f.Result=f.Now>f.FeedLife?1:0;
        else if(f.Operation==2) {
            var c=RuntimeCanvas.current;
            if(c is { } canvas)Draw(canvas,ref f);
        }
        f.Kills=state.Kills;f.Rows=state.Feed.Count;
    }
    static void Draw(RuntimeCanvas c,ref CombatHudFrame f) {
        if(f.AiOverlay)Debug(c,ref f);
        if(!f.PlayerDead) {
            if(f.Armed && f.Magazine>0)Ammo(c,ref f);
            Awareness(c,ref f);
            if(f.God)c.Text(40*c.Scale,c.Height-84*c.Scale,"GOD MODE",24,Color(1,.82f,.25f,.95f));
        }
        Feed(c,ref f);
    }
    static void Ammo(RuntimeCanvas c,ref CombatHudFrame f) {
        float s=c.Scale,right=c.Width-40*s,top=c.Height-50*s,w=260*s,h=10*s;
        int mag=Math.Max(1,f.Magazine);float fraction=Clamp((float)f.Ammo/mag),pulse=.5f+.5f*MathF.Sin(f.RealTime*9);
        bool low=f.Ammo<=Math.Max(1,mag/4),empty=f.Ammo<=0;
        c.Rect(right-w-2*s,top-2*s,w+4*s,h+4*s,Color(0,0,0,.45f));
        var fill=Color(.95f,.95f,.95f,.9f);if(low)fill=Vector4.Lerp(fill,Color(.95f,.15f,.1f,.95f),pulse);
        c.Rect(right-w,top,w*fraction,h,fill);
        string total=f.InfiniteAmmo?"/INF":"/"+f.Magazine;
        float tw=c.Measure(total,34),y=top-8*s-c.LineHeight(84);
        var col=Color(.96f,.96f,.96f,.95f);if(low)col=Vector4.Lerp(col,Color(1,.2f,.12f),pulse);
        c.Text(right-tw-6*s,y,f.Ammo.ToString(),84,col,TextAlignment.Right);
        c.Text(right,y+50*s*.92f,total,34,Color(.8f,.8f,.8f,.85f),TextAlignment.Right);
        c.Text(right,y-4*s,String(f.FireMode),18,Color(.85f,.85f,.85f,.85f),TextAlignment.Right);
        if(f.Reloading)c.Text(right-w,y+6*s,"RELOADING",20,Color(1,.85f,.3f,.6f+.4f*MathF.Sin(f.RealTime*12)));
        else if(empty)c.Text(right-w,y+6*s,"RELOAD",20,Color(1,.25f,.15f,.5f+.5f*MathF.Sin(f.RealTime*10)));
    }
    static void Feed(RuntimeCanvas c,ref CombatHudFrame f) {
        float now=f.Now,life=f.FeedLife,s=c.Scale;
        state.Feed.RemoveAll(r=>now-r.At>life);
        float y=40*s,right=c.Width-40*s,rowH=38*s;
        for(int i=state.Feed.Count-1;i>=0;i--) {
            var r=state.Feed[i];float age=now-r.At,a=Clamp(life-age)*Clamp(age/.08f+.2f);
            float x=right+(1-Clamp(age/.15f))*40*s;string head=r.Head?"HEADSHOT  ":"";
            float w=c.Measure(r.Name,26)+c.Measure(head,26)+c.Measure("ELIMINATED  ",26);
            c.Rect(x-w-12*s,y-3*s,w+26*s,rowH-4*s,Color(0,0,0,.4f*a));
            c.Rect(x+7*s,y-3*s,3*s,rowH-4*s,r.Head?Color(1,.8f,.25f,.9f*a):Color(1,.2f,.15f,.9f*a));
            float cx=x-w;cx+=c.Text(cx,y,"ELIMINATED  ",26,Color(.8f,.8f,.8f,.9f*a));
            if(r.Head)cx+=c.Text(cx,y,head,26,Color(1,.8f,.25f,a));
            c.Text(cx,y,r.Name,26,Color(1,1,1,a));y+=rowH;
        }
        float sage=now-state.StreakAt;
        if(state.Streak>=2 && sage<2.2f) {
            string label=state.Streak==2?"DOUBLE KILL":state.Streak==3?"TRIPLE KILL":"RAMPAGE";
            float a=Clamp((2.2f-sage)/.6f),pop=1+.35f*MathF.Exp(-sage*9);
            var tint=state.Streak>=4?Color(1,.2f,.12f,a):state.Streak==3?Color(1,.55f,.15f,a):Color(1,.85f,.3f,a);
            c.Text(c.Width*.5f,150*s,label,44*pop,tint,TextAlignment.Center);
        }
    }
    static void Awareness(RuntimeCanvas c,ref CombatHudFrame f) {
        var npcs=(NpcHudFrame*)f.Npcs;float s=c.Scale;Vector2 centre=new(c.Width*.5f,c.Height*.5f);
        for(int i=0;i<f.NpcCount;i++) {
            ref var n=ref npcs[i];if(n.Dead || n.Known)continue;float aw=Clamp(n.Awareness);if(aw<.04f)continue;
            float angle=DamageRules.IndicatorAngle(f.Camera,f.CameraYaw,n.Feet);
            Vector2 forward=new(MathF.Sin(angle),-MathF.Cos(angle)),side=new(-forward.Y,forward.X),pos=centre+forward*150*s;
            Vector4 tint=aw<.5f?Vector4.Lerp(Color(.95f,.95f,.95f),Color(1,.85f,.2f),aw/.5f):Vector4.Lerp(Color(1,.85f,.2f),Color(1,.2f,.1f),(aw-.5f)/.5f);
            tint.W=.95f;Vector2 tip=pos+forward*13*s;
            for(int sign=-1;sign<=1;sign+=2) {
                var wing=pos-forward*13*s+side*26*s*.6f*sign;
                c.Line(wing,tip,6*s,Color(0,0,0,.45f));c.Line(wing,tip,4*s,Color(.9f,.9f,.9f,.22f));
                c.Line(wing,wing+(tip-wing)*aw,4*s,tint);
            }
        }
    }
    static Vector4 Clip(ref CombatHudFrame f,Vector3 p) {
        fixed(float* m=f.ViewProjection)return new(m[0]*p.X+m[4]*p.Y+m[8]*p.Z+m[12],m[1]*p.X+m[5]*p.Y+m[9]*p.Z+m[13],m[2]*p.X+m[6]*p.Y+m[10]*p.Z+m[14],m[3]*p.X+m[7]*p.Y+m[11]*p.Z+m[15]);
    }
    static Vector2 Pixel(RuntimeCanvas c,Vector4 p)=>new((p.X/p.W*.5f+.5f)*c.Width,(1-(p.Y/p.W*.5f+.5f))*c.Height);
    static void Debug(RuntimeCanvas c,ref CombatHudFrame f) {
        float s=c.Scale;var lines=(float*)f.DebugLines;
        for(int i=0;i+14<=f.DebugFloatCount;i+=14) {
            Vector3 a=new(lines[i],lines[i+1],lines[i+2]),b=new(lines[i+7],lines[i+8],lines[i+9]);
            if(Vector3.DistanceSquared(a,f.Camera)>6400 && Vector3.DistanceSquared(b,f.Camera)>6400)continue;
            var ca=Clip(ref f,a);var cb=Clip(ref f,b);if(ca.W<.05f && cb.W<.05f)continue;
            if(ca.W<.05f)ca=Vector4.Lerp(ca,cb,(.05f-ca.W)/(cb.W-ca.W));
            if(cb.W<.05f)cb=Vector4.Lerp(cb,ca,(.05f-cb.W)/(ca.W-cb.W));
            c.Line(Pixel(c,ca),Pixel(c,cb),2*s,new(lines[i+3],lines[i+4],lines[i+5],lines[i+6]));
        }
        var npcs=(NpcHudFrame*)f.Npcs;
        for(int i=0;i<f.NpcCount;i++) {
            ref var n=ref npcs[i];if(n.Dead)continue;var head=n.Feet+new Vector3(0,n.Crouched?1.5f:2.05f,0);
            if(Vector3.Distance(head,f.Camera)>70)continue;var clip=Clip(ref f,head);if(clip.W<.05f)continue;
            var p=Pixel(c,clip);string l1=$"{String(n.Name)}  {BehaviourName(n.Behaviour)}  {RoleName(n.Role)}  HP {n.Health:F0}",l2=String(n.Why);
            string l3=$"aw {n.Awareness:F2} {(n.Known?"KNOWN ":"")}{(n.Visible?"SEES ":"")}  sup {n.Suppression:F2}{(n.Attack?"  ATK":"")}{(n.Flank?"  FLK":"")}{(n.Push?"  PSH":"")}";
            float w=Math.Max(c.Measure(l1,15),Math.Max(c.Measure(l2,15),c.Measure(l3,15))),lh=c.LineHeight(15),h=lh*3+6*s,x=p.X,y=p.Y-h-6*s;
            var tint=n.Visible?Color(1,.35f,.3f):n.Known?Color(1,.75f,.2f):n.Awareness>.04f?Color(1,.95f,.4f):Color(.7f,1,.7f);
            c.Rect(x-w*.5f-5*s,y-2*s,w+10*s,h,Color(0,0,0,.55f));var accent=tint;accent.W=.95f;c.Rect(x-w*.5f-5*s,y-2*s,3*s,h,accent);
            c.Text(x,y,l1,15,tint,TextAlignment.Center,false);c.Text(x,y+lh,l2,15,Color(.85f,.85f,.85f),TextAlignment.Center,false);
            c.Text(x,y+lh*2,l3,15,Color(.7f,.85f,1),TextAlignment.Center,false);
        }
        c.Text(40*s,40*s,"AI DEBUG (F9)",18,Color(.55f,.9f,1,.9f));
    }
    static string BehaviourName(int b)=>b switch {0=>"Idle",1=>"Investigate",2=>"Engage",3=>"Take Cover",4=>"Cover Fight",5=>"Flank",6=>"Push",7=>"Search",8=>"Retreat",9=>"Dead",10=>"Wounded",_=>"?"};
    static string RoleName(int r)=>r switch {0=>"Anchor",1=>"Suppressor",2=>"Flanker",_=>"?"};
}
