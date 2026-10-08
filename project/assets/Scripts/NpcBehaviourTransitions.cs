using System.Numerics;
namespace Tartarus.Gameplay;

internal enum NpcCall {Contact,ContactRelay,Gunfire,FlankLeft,FlankRight,MovingUp,FallingBack,LostTarget,ManDown,Wounded,Reloading,Covering,TargetDown,PlayerHurt,Suspicious,Investigating,AllClear,Idle,Melee,Copy}
internal static unsafe partial class NpcBehaviour
{
    static void Callout(NpcBrainFrame n,NpcCall kind)=>Call(n,12,index:(int)kind);
    static void Enter(ref NpcBrainFrame n,EnemyBehaviour behaviour)
    {
        float now=n.Now;Vector3 threat=n.Threat;var previous=(EnemyBehaviour)n.Doing;
        if(previous==EnemyBehaviour.Flank && behaviour!=EnemyBehaviour.Flank && n.Squad<n.SquadCount) {DropFlank(ref n,now);n.HasFlankToken=false;}
        if(previous==EnemyBehaviour.Push && behaviour!=EnemyBehaviour.Push && n.Squad<n.SquadCount) {
            if(n.SquadPushHolder==n.Index) {n.SquadPushHolder=-1;n.SquadPushUntil=0;}n.HasPushToken=false;
        }
        n.Doing=(int)behaviour;n.DoingSince=now;n.BoundWaitFrom=-1;n.Phase=0;n.PhaseStart=now;n.PhaseUntil=now;n.ShotsAtPhase=n.ShotsFired;n.SearchStep=0;
        switch(behaviour) {
        case EnemyBehaviour.TakeCover:
            if(FindCover(ref n,CoverGoal.Fight,threat)<0) {n.Phase=-1;n.NoCoverUntil=now+3;}break;
        case EnemyBehaviour.Flank:
            if(FindCover(ref n,CoverGoal.Flank,threat)<0) {
                n.Phase=-1;if(n.Squad<n.SquadCount)DropFlank(ref n,now);n.HasFlankToken=false;n.FlankFails++;
            } else {
                n.Flanks++;if(n.Squad<n.SquadCount && n.SquadPincerHolder==n.Index)n.Pincers++;
                var c=GetCover(n,n.Cover);Vector3 right=Vector3.Normalize(Vector3.Cross(FlatDir(n.Feet,threat),Vector3.UnitY));
                Callout(n,Vector3.Dot(c.Pos-n.Feet,right)>0?NpcCall.FlankRight:NpcCall.FlankLeft);
                Call(n,13,c.Pos-n.Feet);n.BoundWaitFrom=now;
            }
            break;
        case EnemyBehaviour.Push:
            if(FindCover(ref n,CoverGoal.Push,threat)<0) {n.Cover=-1;Call(n,9);n.Phase=n.Shotgun?2:-1;}
            if(n.Phase>=0) {
                Callout(n,NpcCall.MovingUp);Call(n,13,(n.Cover>=0?GetCover(n,n.Cover).Pos:threat)-n.Feet);
                if(n.Cover>=0)n.BoundWaitFrom=now;
            }
            break;
        case EnemyBehaviour.Retreat:
            n.Retreated=true;if(FindCover(ref n,CoverGoal.Retreat,threat)<0)n.Phase=-1;else Callout(n,NpcCall.FallingBack);break;
        case EnemyBehaviour.Investigate:Callout(n,NpcCall.Investigating);break;
        case EnemyBehaviour.CoverFight:n.PhaseUntil=now+.6f+.8f*Random(n);break;
        case EnemyBehaviour.Search:Callout(n,NpcCall.LostTarget);n.Goal=threat;break;
        case EnemyBehaviour.Idle:
            if(previous is EnemyBehaviour.Investigate or EnemyBehaviour.Search)Callout(n,NpcCall.AllClear);
            Call(n,9);n.Cover=-1;n.IdleUntil=now+1+3*Random(n);break;
        case EnemyBehaviour.Wounded:
            Call(n,9);n.Cover=-1;var query=Call(n,7,n.Feet,value:14);float best=1e9f;int pick=-1;
            for(int pass=0;pass<2 && pick<0;pass++)for(int k=0;k<query.Count;k++) {
                int i=((int*)query.Data)[k];var c=GetCover(n,i);
                if(c.ClaimedBy>=0 && c.ClaimedBy!=n.Index)continue;
                if(pass==0 && Vector3.Dot(c.Normal,FlatDir(c.Pos,threat))<.2f)continue;
                float distance=Flat(n.Feet,c.Pos);if(distance>1 && distance<best) {best=distance;pick=i;}
            }
            if(pick>=0) {Call(n,8,index:pick);n.Cover=pick;n.CoverGood=true;}break;
        }
    }
}
