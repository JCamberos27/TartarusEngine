using System.Numerics;
using System.Runtime.CompilerServices;
namespace Tartarus.Gameplay;
internal enum Gait {Still,Walk,Jog,Run}
internal enum NpcRole {Anchor,Suppressor,Flanker}
internal enum CoverGoal {Fight,Flank,Push,Retreat}
internal static unsafe partial class NpcBehaviour
{
    static float Flat(Vector3 a,Vector3 b)=>new Vector2(a.X-b.X,a.Z-b.Z).Length();
    static Vector3 FlatDir(Vector3 from,Vector3 to) {var d=new Vector3(to.X-from.X,0,to.Z-from.Z);return d.Length()>1e-4f?Vector3.Normalize(d):Vector3.UnitZ;}
    static NpcServiceFrame Call(NpcBrainFrame n,int operation,Vector3 a=default,Vector3 b=default,float value=0,int index=0) {
        NpcServiceFrame request=new() {Operation=operation,A=a,B=b,Value=value,Index=index};
        request.Result=((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)n.Services)(n.Context,&request);return request;
    }
    static float Random(NpcBrainFrame n)=>Call(n,0).Value;
    static bool RandomNear(NpcBrainFrame n,Vector3 a,float radius,float x,float y,out Vector3 p) {var r=Call(n,1,a,new(x,y,0),radius);p=r.C;return r.Result!=0;}
    static bool Closest(NpcBrainFrame n,Vector3 a,out Vector3 p) {var r=Call(n,2,a);p=r.C;return r.Result!=0;}
    static bool Walkable(NpcBrainFrame n,Vector3 a,Vector3 b)=>Call(n,3,a,b).Result!=0;
    static bool Shielded(NpcBrainFrame n,Vector3 a,float height,Vector3 eye)=>Call(n,4,a,eye,height).Result!=0;
    static NpcCoverFrame GetCover(NpcBrainFrame n,int i)=>Call(n,6,index:i).Cover;
    static bool Peek(NpcCoverFrame c,int side)=>side==0?c.Peek0:c.Peek1;
    static Vector3 PeekPos(NpcCoverFrame c,int side)=>side==0?c.PeekLeft:c.PeekRight;
    static void DropFlank(ref NpcBrainFrame n,float now) {if(n.SquadFlankHolder==n.Index) {n.SquadFlankHolder=-1;n.SquadFlankDoneAt=now;}if(n.SquadPincerHolder==n.Index) {n.SquadPincerHolder=-1;n.SquadPincerDoneAt=now;}}
    internal static void Update(NpcBrainFrame* frame) {
        if(frame->Operation==0) {Run(frame);return;}
        var n=*frame;
        if(n.Operation==1)Enter(ref n,(EnemyBehaviour)n.TargetBehaviour);
        else if(n.Operation==2)n.Result=FindCover(ref n,(CoverGoal)n.TargetBehaviour,n.Threat);
        *frame=n;
    }
    internal static void Run(NpcBrainFrame* frame) {
        var n=*frame;float now=n.Now;var prev=n.Intent;NpcIntentFrame it=new() {Pace=(int)Gait.Jog};
        Vector3 threat=n.Threat,threatChest=threat+new Vector3(0,1.3f,0);
        Vector3 aimAt=n.Visible?(n.VisiblePoints>0?n.SeenPoint:n.PlayerFeet+new Vector3(0,n.PlayerHeight*.7f,0)):threatChest;
        bool shotgun=n.Shotgun,reloading=n.Reloading;float ammo01=n.Ammo01,arriveDist=.55f;
        it.LookPoint=n.Known || n.MemoryAwareness>.3f?threatChest:prev.LookPoint;it.AimPoint=aimAt;
    void moveTo(Vector3 goal, Gait pace) {
        it.Move = true;
        it.MoveTarget = goal;
        it.Pace = (int)pace;
    }
    bool arrived(Vector3 goal) => Flat(n.Feet, goal) < arriveDist;
    // Fire and maneuver: a bound across ground the player can see waits - down, gun up, shooting if there's a shot - for a
    // squadmate's covering fire, though not for long if none comes (a push, called while the player reloads, hardly at all).
    // False while it waits.
    bool bound(float maxWait) {
        if (n.BoundWaitFrom < 0.0f || n.Squad >= (int)n.SquadCount) return true;
        
        bool exposed = n.Visible || now - n.LastOwnSight < 0.5f;
        bool covered = now < n.SquadCoverFireUntil;
        if ((!exposed || covered || now-n.BoundWaitFrom>=maxWait)) {
            if (exposed) {
                ++n.Bounds;
                if (covered) ++n.CoveredBounds;
            }
            n.BoundWaitFrom = -1.0f;
            if (n.SquadCoverRequest == n.Index) n.SquadCoverRequest = -1;
            return true;
        }
        n.SquadCoverRequest = n.Index;
        n.SquadCoverRequestAt = now;
        it.Crouch = true;
        it.Aim = true;
        it.FaceAim = true;
        it.Fire = n.Visible;
        return false;
    }
    // Stuck: give the behaviour up and let the next decision pick again.
    if (n.BlockedTime > 1.6f) {
        n.BlockedTime = 0.0f;
        n.Phase = -1;
        Call(n,9);
        n.Cover = -1;
        n.NoCoverUntil = now + 1.5f;
    }

    switch ((EnemyBehaviour)n.Doing) {
    case EnemyBehaviour.Idle: {
        // A slow patrol round its post, pausing to look about.
        it.LookPoint = prev.LookPoint;
        if (n.Phase == 0) {
            if (now >= n.IdleUntil) {
                Vector3 pt=default;
                if (RandomNear(n,n.PostPos, 7.0f, Random(n), Random(n), out pt)) {
                    n.Goal = pt;
                    n.Phase = 1;
                }
                n.IdleUntil = now + 3.0f;
            }
            // Now and then a glance somewhere new; otherwise the look holds.
            if (Random(n) < 0.008f || prev.LookPoint.Length() < 1e-3f) {
                float a = Random(n) * 6.283f;
                it.LookPoint = n.Eye + new Vector3(MathF.Cos(a), -0.1f, MathF.Sin(a)) * 8.0f;
            }
        } else {
            moveTo(n.Goal, Gait.Walk);
            it.LookPoint = n.Goal + new Vector3(0.0f, 1.5f, 0.0f) + FlatDir(n.Feet, n.Goal) * 6.0f;
            if (arrived(n.Goal)) { n.Phase = 0; n.IdleUntil = now + 2.0f + 4.0f * Random(n); }
        }
        break;
    }
    case EnemyBehaviour.Investigate: {
        // Weapon up, walk over to the noise, look round, then a couple of places nearby.
        it.Aim = true;
        it.AimPoint = threatChest;
        it.FaceAim = n.Phase == 1;
        if (n.Phase == 0) {
            moveTo(threat, n.MemoryAwareness > 0.6f ? Gait.Jog : Gait.Walk);
            it.AimPoint = threatChest;
            if (arrived(threat) || Flat(n.Feet, threat) < 2.0f) { n.Phase = 1; n.PhaseUntil = now + 2.5f; }
        } else if (n.Phase == 1) {
            float sweep = MathF.Sin((now - n.PhaseStart) * 1.3f) * 1.1f;
            Vector3 baseDir = FlatDir(n.Feet, threat);
            Vector3 dir=new(baseDir.X * MathF.Cos(sweep) - baseDir.Z * MathF.Sin(sweep), 0.0f, baseDir.X * MathF.Sin(sweep) + baseDir.Z * MathF.Cos(sweep));
            it.AimPoint = n.Eye + dir * 10.0f;
            if (now > n.PhaseUntil) {
                Vector3 pt=default;
                if (n.SearchStep < 2 && RandomNear(n,threat, 6.0f, Random(n), Random(n), out pt)) {
                    n.Goal = pt;
                    n.Phase = 2;
                    ++n.SearchStep;
                } else {
                    n.Phase = -1; // nothing here
                    n.MemoryAwareness = Math.Min(n.MemoryAwareness, 0.25f);
                }
            }
        } else {
            moveTo(n.Goal, Gait.Walk);
            it.AimPoint = n.Goal + new Vector3(0.0f, 1.4f, 0.0f) + FlatDir(n.Feet, n.Goal) * 5.0f;
            if (arrived(n.Goal)) { n.Phase = 1; n.PhaseStart = now; n.PhaseUntil = now + 2.0f; }
        }
        break;
    }
    case EnemyBehaviour.Engage: {
        // In the open: shoot, and keep moving across the line of fire between bursts. A shotgun closes in.
        it.Aim = true;
        it.FaceAim = true;
        it.Fire = true;
        it.Reload = ammo01 <= 0.0f;
        float dist = Flat(n.Feet, threat);
        Vector3 snapped=default;
        if (shotgun && dist > 6.5f) {
            moveTo(threat + FlatDir(threat, n.Feet) * 5.0f, dist > 14.0f ? Gait.Jog : Gait.Walk);
        } else if (!shotgun && dist < 5.0f && (n.Phase != 2 || now > n.PhaseUntil) &&
                   Closest(n,n.Feet + FlatDir(threat, n.Feet) * 3.0f, out snapped) && Walkable(n,n.Feet, snapped)) {
            // Too close for a rifle: back off while shooting.
            if (n.Phase != 2) ++n.Backpedals;
            n.Goal = snapped;
            n.Phase = 2;
            n.PhaseUntil = now + 0.8f;
        } else if (n.Phase == 0 || now > n.PhaseUntil) {
            // A strafe step: 2-4 m across, alternating sides, or a crouch on the spot at range.
            Vector3 across = Vector3.Normalize(Vector3.Cross(FlatDir(n.Feet, threat), new Vector3(0, 1, 0)));
            float side = (n.SearchStep++ % 2 == 0) ? 1.0f : -1.0f;
            Vector3 pt = n.Feet + across * side * (2.0f + 2.0f * Random(n));
            n.Goal = Closest(n,pt, out snapped) && Walkable(n,n.Feet, snapped) ? snapped : n.Feet;
            n.Phase = 1;
            n.PhaseUntil = now + 1.4f + 1.2f * Random(n);
        }
        if ((n.Phase == 1 || n.Phase == 2) && !(shotgun && dist > 6.5f)) {
            if (!arrived(n.Goal)) moveTo(n.Goal, Gait.Walk);
            else it.Crouch = !shotgun && dist > 14.0f && n.SearchStep % 3 == 0;
        }
        break;
    }
    case EnemyBehaviour.TakeCover:
    case EnemyBehaviour.Flank:
    case EnemyBehaviour.Retreat: {
        if (n.Cover < 0) { n.Phase = -1; break; }
        if (!bound(1.6f)) break;
        var c = GetCover(n,n.Cover);
        bool exposed = n.Visible || now - n.LastHurt < 2.0f;
        float left = Flat(n.Feet, c.Pos);
        // Sprint across open ground; jog when hidden; turn and shoot on the way if it's close and in view.
        Gait pace = exposed || left > 12.0f || (EnemyBehaviour)n.Doing == EnemyBehaviour.Retreat ? Gait.Run : Gait.Jog;
        if ((EnemyBehaviour)n.Doing == EnemyBehaviour.Flank && !exposed) pace = left > 8.0f ? Gait.Run : Gait.Jog;
        moveTo(c.Pos, pace);
        if (pace == Gait.Jog && n.Visible && Flat(n.Feet, threat) < 25.0f) {
            it.Aim = true;
            it.FaceAim = true;
            it.Fire = true;
            it.Move = true;
        }
        if (left < arriveDist) {
            // In: fight from here.
            if ((EnemyBehaviour)n.Doing == EnemyBehaviour.Flank && n.Squad < (int)n.SquadCount) {
                DropFlank(ref n,now);
                n.HasFlankToken = false;
            }
            n.Doing = (int)EnemyBehaviour.CoverFight;
            n.DoingSince = now;
            n.Phase = 0;
            n.PhaseStart = now;
            n.PhaseUntil = now + 0.5f + 0.7f * Random(n);
        }
        break;
    }
    case EnemyBehaviour.Push: {
        if (n.Phase == 2 || n.Cover < 0) {
            // A shotgun with nothing to hide behind: straight at them.
            it.Aim = true;
            it.FaceAim = true;
            it.Fire = true;
            moveTo(threat + FlatDir(threat, n.Feet) * 4.0f, Flat(n.Feet, threat) > 10.0f ? Gait.Run : Gait.Jog);
            if (Flat(n.Feet, threat) < 6.0f) n.Phase = -1;
            break;
        }
        if (!bound(0.8f)) break;
        var c = GetCover(n,n.Cover);
        moveTo(c.Pos, Gait.Run);
        if (Flat(n.Feet, c.Pos) < arriveDist) {
            if (n.Squad < (int)n.SquadCount) {
                
                if (n.SquadPushHolder == n.Index) n.SquadPushHolder = -1;
            }
            n.HasPushToken = false;
            n.Doing = (int)EnemyBehaviour.CoverFight;
            n.DoingSince = now;
            n.Phase = 1; // come up shooting
            n.PhaseStart = now;
            n.PhaseUntil = now + 2.0f;
            n.ShotsAtPhase = n.ShotsFired;
        }
        break;
    }
    case EnemyBehaviour.CoverFight: {
        if (n.Cover < 0) { n.Phase = -1; break; }
        var c = GetCover(n,n.Cover);
        float sinceSeen = now - n.MemoryLastSeen;
        if (n.Phase == 0) {
            // Down behind it. Reload here if the magazine's getting light; stay down longer under fire.
            moveTo(c.Pos, Gait.Walk);
            if (Flat(n.Feet, c.Pos) < 0.3f) it.Move = false;
            it.Crouch = !c.High;
            it.Aim = false;
            it.LookPoint = threatChest;
            it.Reload = ammo01 < 0.5f;
            if (n.Suppression > 0.6f) n.PhaseUntil = Math.Max(n.PhaseUntil, now + 0.2f);
            // Told to cover a squadmate's bound: up now.
            bool ordered = now < n.CoverFireOrder;
            if (ordered) n.PhaseUntil = Math.Min(n.PhaseUntil, now);
            // Seen while hiding: the cover's no good any more (flanked) - move.
            if (n.Visible && !n.CoverGood) { n.Phase = -1; Call(n,9); n.Cover = -1; break; }
            // Pinned: the gun up over low cover, or out past the edge of high cover, and a burst without looking.
            if ((!c.High || c.Peek0 || c.Peek1) && !reloading && ammo01 > 0.2f && now - n.LastBlindFire > 5.0f && n.PinnedSince >= 0.0f &&
                (n.Suppression>.5f && now-n.PinnedSince>1.5f && n.Known && sinceSeen<8)) {
                n.Phase = 3;
                n.PhaseStart = now;
                n.PhaseUntil = now + 1.1f + 0.8f * Random(n);
                n.LastBlindFire = now;
                n.ShotsAtPhase = n.ShotsFired;
                n.PeekSide = -1;
                if (c.High) {
                    float bestD = 1e9f;
                    for (int s = 0; s < 2; ++s)
                        if (Peek(c,s) && Flat(PeekPos(c,s), threat) < bestD) { bestD = Flat(PeekPos(c,s), threat); n.PeekSide = s; }
                }
                ++n.BlindFires;
                break;
            }
            if (now > n.PhaseUntil && !reloading && ammo01 >= 0.3f) {
                // Out to shoot: over low cover, else past whichever end sees the threat.
                n.PeekSide = -1;
                if (c.High) {
                    float bestD = 1e9f;
                    for (int s = 0; s < 2; ++s)
                        if (Peek(c,s) && !Shielded(n,PeekPos(c,s), 1.55f, threat + new Vector3(0.0f, 1.6f, 0.0f))) {
                            float dd = Flat(PeekPos(c,s), threat);
                            if (dd < bestD) { bestD = dd; n.PeekSide = s; }
                        }
                    if (n.PeekSide < 0) {
                        // Can't shoot from here: somewhere better.
                        n.Phase = -1;
                        Call(n,9);
                        n.Cover = -1;
                        break;
                    }
                }
                // Peek after peek with nothing to shoot at: this spot is no use - find another.
                if (n.EmptyPeeks >= 2 && !ordered) {
                    n.EmptyPeeks = 0;
                    n.Phase = -1;
                    Call(n,9);
                    n.Cover = -1;
                    break;
                }
                n.Phase = 1;
                n.PhaseStart = now;
                n.ShotsAtPhase = n.ShotsFired;
                n.PhaseUntil = now + 1.6f + 1.6f * Random(n) + ((NpcRole)n.Role == NpcRole.Suppressor ? 0.8f : 0.0f);
            }
        } else if (n.Phase == 1) {
            // Up and shooting: at them when seen, at where they were when suppressing.
            it.Aim = true;
            it.FaceAim = true;
            it.Fire = true;
            it.Crouch = false;
            if (c.High && n.PeekSide >= 0) {
                moveTo(PeekPos(c,n.PeekSide), Gait.Walk);
                if (Flat(n.Feet, PeekPos(c,n.PeekSide)) < 0.25f) it.Move = false;
                it.Lean = n.PeekSide == 0 ? -0.6f : 0.6f;
            }
            if (!n.Visible) {
                bool suppress = ((NpcRole)n.Role == NpcRole.Suppressor || n.SquadPushHolder >= 0 ||
                                       n.SquadFlankHolder >= 0 || n.SquadPincerHolder >= 0 ||
                                       now < n.CoverFireOrder) &&
                                      sinceSeen < 6.0f && !shotgun;
                it.Suppress = suppress;
                it.Fire = suppress;
                if (!suppress && now - n.PhaseStart > 1.2f) n.PhaseUntil = Math.Min(n.PhaseUntil, now);
            }
            if (now < n.CoverFireOrder) n.PhaseUntil = Math.Max(n.PhaseUntil, n.CoverFireOrder);
            int bursts = n.ShotsFired - n.ShotsAtPhase;
            bool hurtNow = n.LastHurt > n.PhaseStart;
            if (now > n.PhaseUntil || hurtNow || bursts >= (shotgun ? 2 : 12) || ammo01 <= 0.0f) {
                n.EmptyPeeks = bursts == 0 && n.LastOwnSight < n.PhaseStart ? n.EmptyPeeks + 1 : 0;
                n.Phase = 0;
                n.PhaseStart = now;
                float baseTime = c.High ? 1.0f : 1.3f;
                n.PhaseUntil = now + baseTime * (0.8f + 1.4f * Random(n)) * (1.0f + n.Suppression) * (1.2f - 0.4f * n.Skill) *
                                         (n.Retreated ? 1.6f : 1.0f);
            }
        } else if (n.Phase == 3) {
            // Blind fire: down behind the cover, head tucked, the gun held out over or round it toward where the player was.
            moveTo(c.Pos, Gait.Walk);
            if (Flat(n.Feet, c.Pos) < 0.3f) it.Move = false;
            it.Crouch = !c.High; // behind high cover it stands, the gun out round the edge
            it.Aim = true;
            it.AimPoint = threatChest;
            it.FaceAim = true;
            it.Fire = true;
            it.Suppress = true;
            it.BlindFire = true;
            it.Cower = 0.8f;
            // Done, empty, hit, or someone at arm's length to strike instead.
            if (now > n.PhaseUntil || ammo01 <= 0.0f || n.LastHurt > n.PhaseStart || n.MeleeAt > n.PhaseStart) {
                it.BlindFire = false;
                n.Phase = 0;
                n.PhaseStart = now;
                n.PhaseUntil = now + 0.8f + 0.8f * Random(n);
            }
        }
        break;
    }
    case EnemyBehaviour.Search: {
        // Hunt: to where they were headed, look round, then spread out over where they could be.
        it.Aim = true;
        it.FaceAim = true;
        if (n.Phase == 0) {
            moveTo(n.Goal, Gait.Jog);
            it.AimPoint = n.Goal + new Vector3(0.0f, 1.3f, 0.0f);
            if (arrived(n.Goal) || n.BlockedTime > 1.0f) { n.Phase = 1; n.PhaseStart = now; n.PhaseUntil = now + 2.2f; }
        } else if (n.Phase == 1) {
            float sweep = MathF.Sin((now - n.PhaseStart) * 1.6f) * 1.3f;
            Vector3 baseDir = FlatDir(n.Feet, threat + new Vector3(0.001f, 0, 0));
            Vector3 dir=new(baseDir.X * MathF.Cos(sweep) - baseDir.Z * MathF.Sin(sweep), 0.0f, baseDir.X * MathF.Sin(sweep) + baseDir.Z * MathF.Cos(sweep));
            it.AimPoint = n.Eye + dir * 10.0f;
            if (now > n.PhaseUntil) {
                Vector3 pt=default;
                float r = Math.Clamp(n.MemoryUncertainty, 4.0f, 14.0f);
                if (n.SearchStep < 4 && RandomNear(n,threat, r, Random(n), Random(n), out pt)) {
                    n.Goal = pt;
                    n.Phase = 0;
                    ++n.SearchStep;
                } else {
                    // Lost them: back to being watchful.
                    n.Known = false;
                    n.MemoryAwareness = 0.4f;
                    n.Phase = -1;
                }
            }
        }
        break;
    }
    case EnemyBehaviour.Wounded: {
        // On a knee, crawling for cover (the director holds its speed to a crawl), shooting when it has the player.
        it.Crouch = true;
        it.Aim = n.Known;
        it.FaceAim = true;
        it.Fire = n.Visible || now - n.LastOwnSight < 1.5f;
        it.Reload = ammo01 <= 0.0f;
        if (n.Cover >= 0) {
            var c = GetCover(n,n.Cover);
            if (Flat(n.Feet, c.Pos) > 0.5f) moveTo(c.Pos, Gait.Walk);
        }
        break;
    }
    case EnemyBehaviour.Dead:
        break;
    }
    // Reactions over whatever the behaviour wants: a duck from a near miss holds the trigger for its beat; a
    // squadmate calling out draws a look while the gun is down.
    if (now < n.CowerUntil) {
        it.Cower = 1.0f;
        it.Fire = false;
    }
    if (now < n.GlanceUntil && !it.Aim) it.LookPoint = n.GlanceAt;
    if (it.Move && Call(n,15).Result==0) it.Move = false;
    n.Intent=it; *frame=n;
}
}
