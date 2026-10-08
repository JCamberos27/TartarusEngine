using System.Numerics;
namespace Tartarus.Gameplay;

internal static unsafe class NpcSquads
{
    static NpcSquadMemberFrame* Get(ref NpcSquadFrame f,int i) {
        var members=(NpcSquadMemberFrame*)f.Members;
        return i>=0 && i<f.TotalCount && members[i].Exists?members+i:null;
    }
    static bool Wants(NpcSquadMemberFrame* n)=>n!=null && !n->Dead && n->Fire && (n->Memory.Visible || n->Suppress);
    static bool Attacks(ref NpcSquadFrame f,int index) {for(int i=0;i<f.AttackerCount;i++)if(f.Attackers[i]==index)return true;return false;}
    static void Sort(ref NpcSquadFrame f,Span<int> ids,Vector3 target) {
        for(int i=1;i<ids.Length;i++) {
            int value=ids[i],j=i-1;float distance=Vector3.Distance(Get(ref f,value)->Feet,target);
            while(j>=0 && Vector3.Distance(Get(ref f,ids[j])->Feet,target)>distance){ids[j+1]=ids[j];j--;}
            ids[j+1]=value;
        }
    }
    internal static void Update(ref NpcSquadFrame f) {
        var ids=(int*)f.MemberIds;NpcMemoryFrame* freshest=null;
        for(int k=0;k<f.MemberCount;k++) {var n=Get(ref f,ids[k]);if(n==null || n->Dead || !n->Memory.Known)continue;if(freshest==null || n->Memory.LastSeen>freshest->LastSeen)freshest=&n->Memory;}
        if(freshest!=null){f.Shared=*freshest;f.SharedAt=f.Now;}
        for(int k=0;k<f.MemberCount;k++) {
            var n=Get(ref f,ids[k]);if(n==null || n->Dead || freshest==null)continue;
            if(f.Shared.LastSeen>n->Memory.LastSeen+.5f && f.Now-f.Shared.LastSeen>.4f) {
                bool was=n->Memory.Known;n->Memory.Known=true;n->Memory.Awareness=1;n->Memory.LastKnown=f.Shared.LastKnown;n->Memory.LastVelocity=f.Shared.LastVelocity;n->Memory.LastSeen=f.Shared.LastSeen;n->Memory.Uncertainty=f.Shared.Uncertainty+1.5f;if(!was)n->ReactionLeft=Math.Max(n->ReactionLeft,.4f);
            }
        }
        if(f.Player.Reloading)for(int k=0;k<f.MemberCount;k++) {
            var n=Get(ref f,ids[k]);if(n!=null && !n->Dead && n->Memory.Known && (n->Memory.Visible || Vector3.Distance(n->SightEye,f.Player.Eye)<12)) {
                if(f.Now-f.PlayerReloadingSeen>3){f.PlayerReloadingSeen=f.Now;f.PushUntil=f.Now+4.5f;}break;
            }
        }
        Span<int> scratch=stackalloc int[f.MemberCount];int count=0;
        if(f.Now>=f.NextRoles) {
            f.NextRoles=f.Now+.5f;for(int k=0;k<f.MemberCount;k++){var n=Get(ref f,ids[k]);if(n!=null && !n->Dead)scratch[count++]=ids[k];}
            var alive=scratch[..count];Sort(ref f,alive,f.Shared.LastKnown);int flanker=-1;float bestScore=-1;
            for(int k=0;k<count;k++) {
                if(count>=2 && k==0)continue;var n=Get(ref f,alive[k]);float score=n->Health/n->MaxHealth+(n->Shotgun?.6f:0)+.2f*n->Skill;
                if(score>bestScore && n->Health>.45f*n->MaxHealth && !n->Wounded){bestScore=score;flanker=alive[k];}
            }
            if(count<2)flanker=-1;
            if(f.PincerHolder>=0){var h=Get(ref f,f.PincerHolder);if(h==null || h->Dead || h->Wounded || f.PincerHolder==flanker)f.PincerHolder=-1;}
            for(int k=0;k<count;k++){var n=Get(ref f,alive[k]);n->Role=alive[k]==flanker || alive[k]==f.PincerHolder?2:k==0 || n->Shotgun?0:1;}
            if(f.FlankHolder>=0){var h=Get(ref f,f.FlankHolder);if(h==null || h->Dead || h->Role!=2)f.FlankHolder=-1;}
            var fl=Get(ref f,flanker);if(f.FlankHolder<0 && fl!=null && fl->Memory.Known && f.Now-f.LastDeath>4 && f.Now-f.FlankDoneAt>8)f.FlankHolder=flanker;
            var first=Get(ref f,f.FlankHolder);
            if(f.PincerHolder<0 && count>=4 && f.Difficulty>=.75f && first!=null && first->Doing==5 && first->Cover>=0 && f.Now-f.LastDeath>4 && f.Now-f.PincerDoneAt>12) {
                int second=-1;float best=-1;
                for(int k=1;k<count;k++){var n=Get(ref f,alive[k]);if(n==first || n->Wounded || !n->Memory.Known || n->Health<.5f*n->MaxHealth || n->Doing!=4)continue;float score=n->Health/n->MaxHealth+.3f*n->Skill;if(score>best){best=score;second=alive[k];}}
                if(second>=0){f.PincerHolder=second;Get(ref f,second)->Role=2;}
            }
            if(f.Player.Valid && !f.Player.Dead && f.Player.Health<.4f && f.Now-f.PlayerHurtPushAt>10 && f.Now>=f.PushUntil)
                for(int k=0;k<count;k++)if(Get(ref f,alive[k])->Memory.Visible){f.PlayerHurtPushAt=f.Now;f.PushUntil=f.Now+4.5f;f.HurtPushes++;break;}
            if(f.Now<f.PushUntil){if(f.PushHolder<0)for(int k=0;k<count;k++){var n=Get(ref f,alive[k]);if(n->Health>.5f*n->MaxHealth && alive[k]!=f.FlankHolder && alive[k]!=f.PincerHolder && !n->Wounded){f.PushHolder=alive[k];break;}}}else f.PushHolder=-1;
            for(int k=0;k<count;k++){var n=Get(ref f,alive[k]);n->HasFlank=alive[k]==f.FlankHolder || alive[k]==f.PincerHolder;n->HasPush=alive[k]==f.PushHolder;}
        }
        CoverFire(ref f);
        if(f.Now>=f.NextTokens) {
            f.NextTokens=f.Now+.25f;int maxShooters=Math.Clamp((int)MathF.Round(1.6f+.9f*f.Difficulty,MidpointRounding.AwayFromZero),1,4),retained=0;
            for(int k=0;k<f.AttackerCount;k++)if(Wants(Get(ref f,f.Attackers[k])))f.Attackers[retained++]=f.Attackers[k];f.AttackerCount=retained;
            count=0;for(int k=0;k<f.MemberCount;k++)if(Wants(Get(ref f,ids[k])) && !Attacks(ref f,ids[k]))scratch[count++]=ids[k];
            Sort(ref f,scratch[..count],f.Player.Feet);for(int k=0;k<count && f.AttackerCount<maxShooters;k++)f.Attackers[f.AttackerCount++]=scratch[k];
            for(int k=0;k<f.MemberCount;k++){var n=Get(ref f,ids[k]);if(n!=null)n->HasAttack=Attacks(ref f,ids[k]);}
        }
    }
    static void CoverFire(ref NpcSquadFrame f) {
        var ids=(int*)f.MemberIds;
        for(int k=0;k<f.MemberCount;k++){var n=Get(ref f,ids[k]);if(n!=null && !n->Dead && ids[k]!=f.CoverRequest && n->TriggerHeld){f.CoverFireUntil=f.Now+.5f;break;}}
        if(f.CoverRequest>=0 && f.Now-f.CoverRequestAt>.3f)f.CoverRequest=-1;
        if(f.CoverFirer>=0){var n=Get(ref f,f.CoverFirer);if(n==null || n->Dead || f.Now>=n->CoverFireOrder)f.CoverFirer=-1;}
        if(f.CoverRequest<0 || f.CoverFirer>=0 || f.Now<f.CoverFireUntil)return;
        int best=-1;float bestScore=0;
        for(int k=0;k<f.MemberCount;k++) {
            var n=Get(ref f,ids[k]);if(n==null || n->Dead || n->Wounded || ids[k]==f.CoverRequest || !n->Memory.Known || n->Reloading || n->Shotgun || n->Doing!=4 || n->Cover<0 || n->EmptyWeapon)continue;
            float score=1+(n->Role==1?.5f:0)+n->Skill*.2f;if(score>bestScore){bestScore=score;best=ids[k];}
        }
        if(best<0)return;var b=Get(ref f,best);b->CoverFireOrder=f.Now+2.5f;b->CallCovering=true;f.CoverFirer=best;f.CoverOrders++;
    }
}
