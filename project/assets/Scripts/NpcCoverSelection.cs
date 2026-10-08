using System.Numerics;
namespace Tartarus.Gameplay;

internal static unsafe partial class NpcBehaviour
{
    // The native backend supplies geometric queries. Ranking, budgets and tactical goals live here.
    [ThreadStatic] static List<(int Index,float Score)>? candidates;
    static int FindCover(ref NpcBrainFrame n,CoverGoal goal,Vector3 threat)
    {
        if(n.CoverCount==0)return -1;
        n.SearchCoverCalled=1;
        Vector3 eye=threat+new Vector3(0,1.6f,0),centroid=n.Feet;
        float current=Flat(n.Feet,threat);int count=1;
        for(int i=0;i<n.MateCount;i++) {
            var r=Call(n,10,index:i);var mate=r.Mate;
            if(r.Result!=0 && !mate.Dead && mate.Index!=n.Index && mate.Squad==n.Squad) {centroid+=mate.Feet;count++;}
        }
        centroid/=count;Vector3 squadDir=FlatDir(threat,centroid),otherFlank=default;bool pincer=false;
        if(goal==CoverGoal.Flank)for(int i=0;i<n.MateCount;i++) {
            var r=Call(n,10,index:i);var mate=r.Mate;
            if(r.Result!=0 && !mate.Dead && mate.Index!=n.Index && mate.Squad==n.Squad && mate.Doing==(int)EnemyBehaviour.Flank && mate.Cover>=0) {
                otherFlank=FlatDir(threat,GetCover(n,mate.Cover).Pos);pincer=true;break;
            }
        }
        var query=Call(n,7,n.Feet,value:goal==CoverGoal.Flank?34:28);
        var list=candidates??=new();list.Clear();
        float* scores=stackalloc float[5];
        for(int k=0;k<query.Count;k++) {
            int i=((int*)query.Data)[k];var c=GetCover(n,i);
            if(c.ClaimedBy>=0 && c.ClaimedBy!=n.Index)continue;
            bool crowded=false;
            for(int j=0;j<n.MateCount;j++) {
                var r=Call(n,10,index:j);var mate=r.Mate;
                if(r.Result==0 || mate.Dead || mate.Index==n.Index)continue;
                if(Flat(mate.Feet,c.Pos)<2.2f || (mate.Cover>=0 && Flat(GetCover(n,mate.Cover).Pos,c.Pos)<3)) {crowded=true;break;}
            }
            if(crowded)continue;
            float facing=Vector3.Dot(c.Normal,FlatDir(c.Pos,threat));if(facing<.35f)continue;
            float dist=Flat(c.Pos,threat);if(dist<4)continue;
            float travel=Flat(n.Feet,c.Pos),pref=n.Shotgun?9:21,width=n.Shotgun?5:11,rd=(dist-pref)/width;
            scores[0]=MathF.Exp(-rd*rd)*.8f+.2f;scores[1]=Math.Clamp(1-travel/32,.05f,1);
            scores[2]=.5f+.5f*facing;scores[3]=n.Now-c.LastUsed<6?.6f:1;scores[4]=c.High && !c.Peek0 && !c.Peek1?.55f:1;
            AiRuleFrame combine=new() {Operation=11,Count=5,Values=scores};AiRules.Update(ref combine);
            AiRuleFrame danger=new() {Operation=9,Position=c.Pos,Positions=n.Deaths,Times=n.DeathTimes,Count=n.DeathCount,Now=n.Now,Radius=5,Memory=30};AiRules.Update(ref danger);
            float score=combine.Result*danger.Result;
            switch(goal) {
            case CoverGoal.Flank:
                float angle=MathF.Acos(Math.Clamp(Vector3.Dot(FlatDir(threat,c.Pos),squadDir),-1,1))*180/MathF.PI;
                if(angle<45 || dist>current+4)continue;
                score*=.4f+.6f*Math.Clamp((angle-45)/45,0,1);
                if(pincer && MathF.Acos(Math.Clamp(Vector3.Dot(FlatDir(threat,c.Pos),otherFlank),-1,1))*180/MathF.PI<80)continue;
                break;
            case CoverGoal.Push:
                if(dist>current-3.5f)continue;score*=.6f+.4f*Math.Clamp((current-dist)/10,0,1);break;
            case CoverGoal.Retreat:
                if(dist<current+3)continue;score*=scores[1];break;
            }
            list.Add((i,score));
        }
        list.Sort(static(a,b)=>b.Score.CompareTo(a.Score));int maxTests=goal==CoverGoal.Flank?16:6,tested=0;
        foreach(var candidate in list) {
            if(++tested>maxTests)break;
            var c=GetCover(n,candidate.Index);
            if(!Shielded(n,c.Pos,c.High?1.55f:.95f,eye))continue;
            if(goal!=CoverGoal.Retreat) {
                bool shoot=!c.High && !Shielded(n,c.Pos,1.55f,eye);
                if(c.High)for(int side=0;side<2 && !shoot;side++)shoot=Peek(c,side) && !Shielded(n,PeekPos(c,side),1.55f,eye);
                if(!shoot)continue;
            }
            float path=Call(n,5,n.Feet,c.Pos).Value;
            if(path<0 || path>Flat(n.Feet,c.Pos)*1.8f+5)continue;
            Call(n,8,index:candidate.Index);n.Cover=candidate.Index;n.CoverGood=true;n.CoverCheckAt=n.Now;return candidate.Index;
        }
        return -1;
    }
}
