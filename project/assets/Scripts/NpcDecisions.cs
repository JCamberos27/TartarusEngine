namespace Tartarus.Gameplay;

internal enum EnemyBehaviour {Idle,Investigate,Engage,TakeCover,CoverFight,Flank,Push,Search,Retreat,Dead,Wounded}
internal static unsafe class NpcDecisions
{
    internal static void Choose(ref AiChoiceFrame f) {
        f.NeedsCoverCheck=f.Wounded==0 && f.Cover>=0 && f.Now-f.CoverCheckAt>.5f && f.Known!=0?1:0;
        if(f.Operation==0)return;
        f.NextThink=f.Now+.25f+.08f*f.Random;f.Change=f.Delay=0;
        for(int i=0;i<11;i++)f.Scores[i]=0;
        if(f.Wounded!=0) {f.Best=10;f.Change=f.Doing!=10 || f.Phase<0?1:0;return;}
        float since=f.Now-f.LastSeen,health=f.Health/Math.Max(f.MaxHealth,1);
        bool underFire=f.Now-f.LastHurt<2.5f || f.Suppression>.45f;
        bool inCover=f.Cover>=0 && (f.DistanceToCover<.9f || (f.Doing==4 && f.Phase==1));
        if(f.Known==0) {
            f.Scores[0]=f.Awareness<.3f && f.Now-f.LastHeard>6?.35f:.05f;
            if(f.Awareness>=.3f || f.Now-f.LastHeard<8)f.Scores[1]=.5f+.4f*f.Awareness;
        } else {
            float searchAfter=7+6*(1-f.Morale)+(f.Role==0?4:0);
            if(f.Visible==0 && since>searchAfter)f.Scores[7]=.45f+.4f*Math.Clamp((since-searchAfter)/15,0,1);
            if(!inCover && f.Now>=f.NoCoverUntil) {
                float cover=.45f+(f.Visible!=0 || underFire?.35f:0);
                if(f.Shotgun!=0 && f.Distance<9 && f.Visible!=0)cover*=.45f;f.Scores[3]=cover;
            }
            if(inCover)f.Scores[4]=f.CoverGood!=0?.72f:.12f;
            float ownSince=f.Now-f.LastOwnSight;
            if(f.Visible!=0 || ownSince<1) {
                float engage=.35f;if(f.Shotgun!=0 && f.Distance<11)engage+=.4f;
                if(f.Now<f.NoCoverUntil)engage+=.25f;if(f.Visible==0)engage*=.6f;f.Scores[2]=engage;
            }
            if(f.Visible==0 && ownSince>3 && !inCover && f.Now<f.NoCoverUntil)f.Scores[7]=Math.Max(f.Scores[7],.55f);
            if(f.HasFlank!=0 && since<20)f.Scores[5]=.62f+.3f*Math.Clamp((f.PlayerStill-4)/6,0,1)+(f.PlayerUnseen>3?.12f:0);
            if(f.HasPush!=0)f.Scores[6]=.82f;
            else if(f.Shotgun!=0 && f.Distance>13 && health>.5f && since<12)f.Scores[6]=.6f;
            if(health<.35f && underFire && f.Retreated==0)f.Scores[8]=.88f;
        }
        if(f.Now<f.LimpUntil)f.Scores[5]=f.Scores[6]=0;
        if(f.Phase>=0 && f.Doing!=9) {
            float commit=f.Doing is 3 or 5 or 6 or 8?1.2f:.6f;
            if(f.Scores[f.Doing]>0) {
                f.Scores[f.Doing]+=.12f;
                if(f.Now-f.DoingSince<commit && f.Now-f.LastHurt>.2f)f.Scores[f.Doing]+=.5f;
            }
        } else if(f.Phase<0)f.Scores[f.Doing]*=.2f;
        int best=0;for(int i=1;i<11;i++)if(f.Scores[i]>f.Scores[best])best=i;f.Best=best;
        if(best!=f.Doing || f.Phase<0) {
            if(best is 3 or 5 or 6 or 8 && f.CoverSearchBlocked!=0) {f.Delay=1;f.NextThink=f.Now;}
            else f.Change=1;
        }
    }
}
