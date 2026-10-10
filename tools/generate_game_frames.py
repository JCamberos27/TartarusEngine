"""Generate sample-project adapter frames. These are game contracts, not engine SDK types."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
schema={'PlayerFrame': {'float': 'Dt Yaw Pitch MoveX MoveY LookPitch LookYaw SizeX '
                          'SizeY EyeHeight MoveSpeed SprintMultiplier '
                          'JumpSpeed Gravity MouseSensitivity '
                          'StickLookDegPerSec KillY RootMotionWeight '
                          'MaxYawRate YawFreeCenter YawFreeRange CrouchHeight '
                          'CrouchSpeedMultiplier JumpBufferTime CoyoteTime '
                          'GroundAccelTime GroundDecelTime AirAccelTime '
                          'SinceGrounded JumpBuffer CrouchBlend CrouchBlendRate YawDropped',
                 'Vec3': 'Position Velocity RespawnFeet RootMotionVelocity '
                         'WishVelocity',
                 'int': 'ReadInput Sprint JumpDown Crouch AimHeld Grounded '
                        'Crouched Jumped'},
 'WeaponFrame': {'float': 'Dt Rpm CycleDelay HoldSeconds RegripMin RegripMax '
                          'Cooldown CycleWait IdleTime RegripDelay SinceShot '
                          'SinceUnhidden ReloadHeldSeconds Random',
                 'int': 'Operation Ammo Magazine BurstRounds AllowFullAuto '
                        'PerRound CycleAfterShot HipProcedural RecoilProfile '
                        'UnityRecoil Equipped Chambered CycleSeen StopReload '
                        'FullAuto BurstRemaining WaitingShot Tags InTransition '
                        'WallBlocked Pressed Held ReloadWasDown '
                        'ReloadHoldFired Events Commands Result'},
 'ShotFrame': {'Vec3': 'Origin Direction',
               'float': 'Spread ImpactImpulse ImpactMaxSpeed BulletHoleRadius '
                        'Range',
               'int': 'Pellets RandomSeed'},
 'NpcCoverFrame': {'Vec3': 'Pos Normal PeekLeft PeekRight',
                   'float': 'LastUsed',
                   'bool32': 'High Peek0 Peek1',
                   'int': 'ClaimedBy'},
 'NpcMateFrame': {'Vec3': 'Feet',
                  'bool32': 'Dead',
                  'int': 'Index Squad Doing Cover'},
 'NpcIntentFrame': {'Vec3': 'MoveTarget AimPoint LookPoint',
                    'float': 'Lean Cower',
                    'bool32': 'Move Crouch Aim FaceAim Fire Suppress Reload '
                              'BlindFire',
                    'int': 'Pace'},
 'NpcServiceFrame': {'Vec3': 'A B C',
                     'float': 'Value',
                     'int': 'Operation Index Count Result',
                     'ptr': 'Data',
                     'NpcCoverFrame': 'Cover',
                     'NpcMateFrame': 'Mate'},
 'NpcBrainFrame': {'float': 'Now Ammo01 PlayerHeight MemoryAwareness '
                            'MemoryLastSeen MemoryUncertainty DoingSince '
                            'BoundWaitFrom PhaseStart PhaseUntil NoCoverUntil '
                            'IdleUntil BlockedTime LastOwnSight LastHurt '
                            'Suppression CoverFireOrder PinnedSince '
                            'LastBlindFire Skill MeleeAt CowerUntil '
                            'GlanceUntil CoverCheckAt SquadCoverFireUntil '
                            'SquadCoverRequestAt SquadPushUntil '
                            'SquadFlankDoneAt SquadPincerDoneAt',
                   'Vec3': 'Feet Eye Goal PostPos GlanceAt Threat PlayerFeet '
                           'SeenPoint',
                   'bool32': 'Known Visible Shotgun Reloading Retreated '
                             'CoverGood HasFlankToken HasPushToken',
                   'int': 'Operation TargetBehaviour Doing Phase ShotsAtPhase '
                          'ShotsFired SearchStep Squad Index Cover Role '
                          'PeekSide EmptyPeeks VisiblePoints SquadCount '
                          'MateCount CoverCount SquadPushHolder '
                          'SquadFlankHolder SquadPincerHolder '
                          'SquadCoverRequest DeathCount Result '
                          'SearchCoverCalled Bounds CoveredBounds Backpedals '
                          'BlindFires Flanks FlankFails Pincers',
                   'ptr': 'Services Context',
                   'vectors': 'Deaths',
                   'floats': 'DeathTimes',
                   'NpcIntentFrame': 'Intent'},
 'VitalsFrame': {'float': 'MaxHealth RegenDelay RegenRate RespawnDelay '
                          'SpawnProtection Health SinceHit DeadTime Protection '
                          'DamageTaken HurtFlash Hitmarker Dt Amount Taken '
                          'Health01 RespawnProgress DeathFade DeathDrop '
                          'DeathRoll EyeHeight CameraYaw',
                 'int': 'Operation GodMode Dead JustDied DiedThisTick Deaths '
                        'HitmarkerKill HitmarkerHead Indicators WantsRespawn',
                 'Vec3': 'Source',
                 'float8': 'SourceX SourceY SourceZ Age Strength ArcAngle '
                           'ArcAlpha'},
 'DamageFrame': {'float': 'Distance Start End MinScale Damage HeadMultiplier '
                          'LimbMultiplier HitY FootY Height Yaw Result',
                 'int': 'Operation Zone Region Part',
                 'Vec3': 'Camera Source'},
 'GravityFrame': {'float': 'Dt Yaw Scroll GrabRange AssistRange AssistConeDeg '
                           'ScrollTurnDeg MinThrowSpeed MaxThrowSpeed '
                           'ChargeTime Backspin HoldDistance Charge HoldYaw '
                           'RotationW PredictSpeed',
                  'int': 'Operation Left Right Alt Control LeftPrev RightPrev '
                         'Charging HaveHoldPoint Handle Held',
                  'Vec3': 'Eye Forward CameraRight PrevHoldPoint HoldVelocity '
                          'Rotation'},
 'AiRuleFrame': {'float': 'Distance Angle Speed Suppression Alertness Focal '
                          'Peripheral Range BaseRate Dt Now Rate Awareness '
                          'LastSeen LastHeard Uncertainty TimeOnTarget '
                          'SelfSpeed Skill Difficulty VisibleFraction Random '
                          'Waited MaxWait PinnedFor SinceSeen Facing '
                          'SinceStrike Reach Cooldown Radius Memory M K B C '
                          'Result',
                 'int': 'Operation VisiblePoints Crouched Firing Known Visible '
                        'BecameKnown OutsideView Flinching Weapon Exposed '
                        'CoverFire KnowsThreat CurveKind Count',
                 'Vec3': 'Position Velocity Listener Source',
                 'floats': 'Values Times',
                 'vectors': 'Positions'},
 'AiChoiceFrame': {'float': 'Now LastSeen LastHeard Health MaxHealth '
                            'Suppression LastHurt Awareness Morale Distance '
                            'DistanceToCover CoverCheckAt LastOwnSight '
                            'NoCoverUntil PlayerStill PlayerUnseen LimpUntil '
                            'DoingSince NextThink Random',
                   'int': 'Operation Known Visible Wounded Doing Phase Role '
                          'Shotgun Cover CoverGood HasFlank HasPush Retreated '
                          'Best NeedsCoverCheck CoverSearchBlocked Change '
                          'Delay',
                   'float11': 'Scores'},
 'NpcDamageFrame': {'float': 'Now Amount Health MaxHealth LastHurt Suppression '
                             'TimeOnTarget Awareness LastSeen Uncertainty '
                             'ReactionLeft LimpUntil StaggerUntil NextThink '
                             'LimpTime HeavyHitDamage StaggerTime WoundChance '
                             'Random Shove',
                    'int': 'Operation UseHealth Entity Dead Wounded Attacker Zone Part Hits '
                           'Known Visible WoundRolled Dummy Ignore Kill Flinch '
                           'NeedRandom BecomeWounded CallWounded',
                    'Vec3': 'Direction LastHurtFrom LastKnown PlayerFeet '
                            'PushVelocity'}}
schema['NpcHudFrame']={'Vec3':'Feet','float':'Health Awareness Suppression','bool32':'Dead Known Visible Crouched Attack Flank Push','int':'Behaviour Role','ptr':'Name Why'}
schema['CombatHudFrame']={'Vec3':'Camera','float':'Now RealTime CameraYaw FeedLife StreakWindow','bool32':'Head PlayerDead Armed Reloading God InfiniteAmmo AiOverlay','int':'Operation Unit Ammo Magazine NpcCount DebugFloatCount Kills Rows Streak Result','ptr':'FireMode Npcs DebugLines','float16':'ViewProjection'}
schema['NpcMemoryFrame']={'bool32':'Known Visible','float':'Awareness LastSeen LastHeard Uncertainty','Vec3':'LastKnown LastVelocity'}
schema['NpcPlayerFrame']={'bool32':'Valid Crouched Dead Fired Reloading Sprinting','float':'Height Radius Health','Vec3':'Eye Feet Velocity Forward'}
schema['NpcNoiseFrame']={'Vec3':'Position','float':'Radius Loudness Time','int':'Source'}
schema['NpcPerceptionMateFrame']={'bool32':'Exists','int':'Squad','NpcMemoryFrame':'Memory'}
schema['NpcPerceptionFrame']={'NpcMemoryFrame':'Memory','NpcPlayerFrame':'Player','Vec3':'Eye SightEye SeenPoint','float':'Now Dt Suppression ReactionLeft PinnedSince Skill Difficulty AimYaw AimPitch LookYaw LookPitch NextLook LastOwnSight CowerUntil TimeOnTarget','bool32':'Aim FirstShot','int':'NoiseCount MateCount Index Squad Doing VisiblePoints Calls Startles','ptr':'Noises Mates Services Context'}
schema['NpcSquadMemberFrame']={'NpcMemoryFrame':'Memory','Vec3':'Feet SightEye','float':'Health MaxHealth Skill ReactionLeft CoverFireOrder','bool32':'Exists Dead Wounded Shotgun Fire Suppress TriggerHeld Reloading EmptyWeapon HasFlank HasPush HasAttack CallCovering','int':'Role Doing Cover'}
schema['NpcSquadFrame']={'NpcMemoryFrame':'Shared','NpcPlayerFrame':'Player','float':'Now Difficulty SharedAt PlayerReloadingSeen PushUntil NextRoles LastDeath FlankDoneAt PincerDoneAt PlayerHurtPushAt CoverRequestAt CoverFireUntil NextTokens','int':'TotalCount MemberCount FlankHolder PincerHolder PushHolder CoverRequest CoverFirer HurtPushes CoverOrders AttackerCount','int4':'Attackers','ptr':'Members MemberIds'}
schema['NpcCombatFrame']={'NpcPlayerFrame':'Player','NpcIntentFrame':'Intent','Vec3':'Eye Feet Velocity GunVelocity DamagePoint DamageDirection','float':'Now Dt Skill Difficulty MeleeAt MeleeTime MeleeHitTime MeleeDamage BodyYaw LookYaw LookPitch AimYaw AimPitch AimYawRate AimPitchRate TimeOnTarget Error ReactionLeft BlockedTime BurstPause LastShot Damage','bool32':'Reloading Pumping AimGun Visible Known HoldFire Dummy Wounded HasAttackToken Signalling Equipped Shotgun TriggerHeld Chambered FirstShot FullAutoSet AllowFullAuto IsFullAuto MeleeLanded Pressed Held Reload ToggleAuto GunNoise Sprinting CallMelee WeaponActive','int':'Operation Ammo AmmoSeen ShotsFired BurstLeft Melees MeleeHits WeaponStateLength','ptr':'Services Context WeaponState'}
schema['AudioShotFrame']={'double':'Now LastShot LastTail','float':'PitchMin PitchMax Random Distance Volume PlayerGain TailMinInterval TailDuckPerVoice BurstGap Pitch','bool32':'At2D','int':'Count TailVoices','int5':'Player2D Every Play','float5':'Near Far NearWeight FarWeight Gain'}
schema['FoleyFrame']={'float':'SprintStride WalkStride StepStrideScale CrouchStrideScale FallSpeed LandMinSpeed LandFullSpeed LandVolume LandHeavySpeed Speed RunSpeed CrouchVolume RunVolume WalkVolume NpcStepVolume Result Gain','bool32':'Sprinting Crouched','int':'Operation Element'}
schema['BodyMotionFrame']={'Vec3':'Velocity WishVelocity Move LastDir StartDir StopDir PivotDir StepDir','float':'Dt ViewYaw Yaw Twist AirTime IdleTime MoveTime TurnTime TurnDone RootYaw StateTime PlayerRunSpeed PlayerSprintSpeed RunSpeed ClipSprint Responsiveness MaxPlayRate ParamSmoothing TurnThreshold TurnMinTime TurnEndAngle TurnTimeout TurnMoveEase TurnLagMargin TurnLagFloor StopDebounce StartIdleTime StartMaxMove StopMinRunTimeCrouched StopMinRunTime StopMinSpeedCrouched StopMinSpeed StopRunForward AirborneDelay PlayRate TurnAngle AccelTime DecelTime Random StartGait StopGait PivotGait StartTurn StartTurnAmount StartDistance StopDistance PivotDistance PivotTravel FidgetTime FidgetNext FidgetIndex SprintRate SprintClip MoveDistance','bool32':'Grounded Crouched WasCrouched StartStopClips Turning IsTurn IsCrouchTurn IsLocomotion IsCrouchLoco Jumped Still SetTurnAngle LastSprint Moving Sprint Airborne Armed Busy IsStart IsStartTurn IsStop IsPivot IsStep IsFidget IsSprint PivotReversed','int':'Triggers'}
schema['NpcCallMemberFrame']={'Vec3':'Feet Eye GlanceAt','float':'LastCallout GlanceUntil Awareness NextChatter','bool32':'Exists Dummy Dead Fire Known Reloading Suppress TriggerHeld VcReloading VcCovering VcSuspicious','int':'Index Squad Behaviour'}
schema['NpcCallChannelFrame']={'float':'BusyUntil RespAt','int':'OnAirPriority','bool32':'RespPending','float20':'LastEvent'}
schema['NpcCalloutsFrame']={'NpcPlayerFrame':'Player','float':'Now','bool32':'PlayerWasDead','int':'Operation Caller Kind Random MemberCount ChannelCount','ptr':'Members Channels'}
schema['WardrobeRandomFrame']={'ptr':'Input Context Services'}
schema['NpcSpawnFrame']={'Vec3':'Position Feet Eye LookPoint','float':'Now Yaw AimYaw NextLook NextThink Radius StandCylinder AgentRadius AgentHeight JogSpeed RunSpeed','int':'Index Weapon Random Count','ptr':'Members'}
schema['NpcLifeFrame']={'NpcIntentFrame':'Intent','NpcMemoryFrame':'Memory','Vec3':'Feet Eye Want Velocity Push Displacement Origin End','float':'Now Dt WoundedAt DiedAt BleedOutTime CorpseTime CrawlSpeed LimpScale LimpUntil StaggerUntil Speed Gravity FallSpeed BlockedTime MeleeAt MeleeTime PlayerDistance FootIKRange StandCylinder CrouchCylinder Suppression CowerUntil Skill Morale','bool32':'Dead Wounded Kill Despawn Crouch Aim Sprint FootIK FootIKEverywhere OnScreen Grounded CallManDown','int':'Operation','ptr':'Services Context'}
schema['NpcPlayerNoiseFrame']={'NpcPlayerFrame':'Player','NpcNoiseFrame':'Gun Step Reload','Vec3':'Post','float':'Now Dt FootstepTimer Still','int':'Count'}
schema['NpcRespawnPointFrame']={'Vec3':'Position','bool32':'Seen'}
schema['NpcRespawnFrame']={'NpcPlayerFrame':'Player','float':'Now Timer','int':'Alive Wanted Count Best','bool32':'Ready Discard','ptr':'Points'}
schema['NpcShotPoseFrame']={'NpcPlayerFrame':'Player','NpcIntentFrame':'Intent','NpcMemoryFrame':'Memory','Vec3':'Feet Eye Velocity SeenPoint BlindOffset CoverPosition Peek CameraOffset Shot','float':'Now Dt AimYaw AimPitch BlindLift MeleeAt MeleeTime TimeOnTarget Suppression Skill Difficulty LastHurt Roll','bool32':'Alive HighCover TriggerHeld FirstShot Wounded HaveShot','int':'VisiblePoints Weapon','ptr':'Services Context'}
schema['DevFrame']={'bool32':'Open InfiniteAmmo Invisible AiOverlay God Frozen HoldFire HaveBloodLab BloodLab','float':'Difficulty TimeScale','int':'Operation Count Alive Known Wanted Spawns Result','ptr':'Members Services Context'}
schema['GameSessionFrame']={'bool32':'HasInput GravityLive Dead Equipped FirePressed FireHeld ReloadHeld Invisible InfiniteAmmo PlayerValid Armed JustDied WantsRespawn NpcHit AliveHit Killed Head','Vec3':'Position Velocity Camera RespawnFeet','float':'Scroll EyeHeight','int':'Operation Commands SlotDirection Ammo Magazine'}
schema['FoleyFrame']['float']+=' Dt PrevVy Vy MinStepSpeed FootLiftMoving FootLiftHeight'
schema['FoleyFrame']['bool32']+=' Grounded PrevGrounded Jumped HaveFootHeights StepsFromFeet Landing ResetFeet ResetSteps UseFeet UseDistance'
schema['NpcShotEffectFrame']={'NpcPlayerFrame':'Player','Vec3':'Origin End Eye Closest ReloadPosition PumpPosition','float':'Reach Miss Suppression Damage DamageScale','bool32':'FirstPellet Shotgun HitPlayer Reloading Pumping WasReloading WasPumping Friendly','int':'Operation Commands Tracer'}
schema['ImpactFrame']={'double':'Now Last Interval','float':'Speed MinSpeed FullSpeed Volume GainMin Gain Radius ShellRadius Miss FlybyRadius FlybyVolume FarGain','bool32':'Enabled CasingsEnabled','int':'Operation Contact MaxContacts Result'}
schema['GameSessionFrame']['bool32']+=' AimHeld SprintHeld'
schema['WeaponLoadoutFrame']={'bool32':'Active Equipped Requested Chambered Hidden InTransition','float':'CycleWait','int':'Operation Value Count Slot Pending Ammo Magazine Burst Commands','ptr':'SlotAmmo'}
cpp=['// Generated by tools/generate_game_frames.py. Project adapter ABI, not engine SDK.','#pragma once','#include "ScriptAbi.h"','namespace Scripting {']
cs=['// Generated by tools/generate_game_frames.py. Project adapter ABI, not engine SDK.','using System.Numerics;','using System.Runtime.InteropServices;','namespace Tartarus.Gameplay;']
for name,groups in schema.items():
 cpp.append('struct '+name+' {');cs+=['[StructLayout(LayoutKind.Sequential)]',('public' if name in ['PlayerFrame','WeaponFrame','ShotFrame'] else 'internal')+' unsafe struct '+name+' {']
 for kind,names in groups.items():
  for field in names.split():
   if kind=='bool32':
    cpp.append(f'    std::int32_t {field}{{}};');cs.append(f'    private int _{field}; public bool {field} {{get=>_{field}!=0;set=>_{field}=value?1:0;}}')
   elif kind.startswith('int') and kind[3:].isdigit():
    count=int(kind[3:]);cpp.append(f'    std::int32_t {field}[{count}]{{}};');cs.append(f'    public fixed int {field}[{count}];')
   elif kind.startswith('float') and kind[5:].isdigit():
    count=int(kind[5:]);cpp.append(f'    float {field}[{count}]{{}};');cs.append(f'    public fixed float {field}[{count}];')
   else:
    cpp.append(f'    {dict(double="double",float="float",int="std::int32_t",Vec3="Vec3",floats="const float*",vectors="const Vec3*",ptr="std::uintptr_t").get(kind,kind)} {field}{{}};')
    cs.append(f'    public {dict(double="double",float="float",int="int",Vec3="Vector3",floats="float*",vectors="Vector3*",ptr="nint").get(kind,kind)} {field};')
 cpp.append('};');cs.append('}')
cpp.append('}')
(root/'src/Game/Scripting/GameFrames.h').write_text('\n'.join(cpp)+'\n')
(root/'project/assets/Scripts/GameFrames.cs').write_text('\n'.join(cs)+'\n')
