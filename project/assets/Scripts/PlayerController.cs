using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

/// <summary>Player gameplay. PhysX remains a native service, called only on the game thread.</summary>
public sealed class PlayerController
{
    static float Wrap(float d) { d %= 360; return d > 180 ? d - 360 : d <= -180 ? d + 360 : d; }
    static bool Fits(float height) => PlayerCharacter.Fits(height);
    static bool Resize(float height) => PlayerCharacter.Resize(height);
    static Vector3 Feet() => PlayerCharacter.Feet;
    static void SetFeet(Vector3 feet) => PlayerCharacter.Feet = feet;
    static int Move(Vector3 displacement, float dt) => PlayerCharacter.Move(displacement, dt);
    public static Vector3 Approach(Vector3 current, Vector3 target, float dt, float accel, float decel)
    {
        float t = target.Length() >= current.Length() ? accel : decel;
        if (!(t > 0) || !(dt > 0)) return target;
        Vector3 next = current + (target - current) * (1 - MathF.Exp(-dt / t));
        return Vector3.Distance(target, next) < .01f ? target : next;
    }
    public void Update(ref PlayerFrame p)
    {
        float dt = p.Dt;
        p.YawDropped = 0;
        if (p.ReadInput != 0)
        {
            float before = p.Yaw;
            // Native camera clamps each mouse/stick pass; the input adapter provides their combined angles.
            p.Yaw += p.LookYaw;
            p.Pitch = Math.Clamp(p.Pitch + p.LookPitch, -89, 89);
            if (p.MaxYawRate > 0 && dt > 0)
            {
                float reach = Math.Max(MathF.Abs(Wrap(before - p.YawFreeCenter)), p.YawFreeRange) + p.MaxYawRate * dt;
                float after = Wrap(p.Yaw - p.YawFreeCenter);
                if (MathF.Abs(after) > reach) { p.YawDropped = MathF.Abs(after) - reach; p.Yaw = p.YawFreeCenter + MathF.CopySign(reach, after); }
            }
        }
        float yaw = p.Yaw * MathF.PI / 180;
        Vector3 forward = new(MathF.Cos(yaw), 0, MathF.Sin(yaw)), right = new(-MathF.Sin(yaw), 0, MathF.Cos(yaw));
        Vector3 wish = right * p.MoveX + forward * p.MoveY;
        if (wish.LengthSquared() > 1) wish = Vector3.Normalize(wish);
        p.MoveX = Vector3.Dot(wish, right); p.MoveY = Vector3.Dot(wish, forward);
        float radius = Math.Max(.05f, p.SizeX * .5f);
        float stand = Math.Max(.05f, p.SizeY * .5f - radius), crouch = Math.Max(.05f, p.CrouchHeight * .5f - radius);
        bool hasCharacter = PlayerCharacter.Exists;
        if (p.CrouchHeight <= 0)
        {
            if (p.Crouched != 0 && hasCharacter) Resize(stand);
            p.Crouched = 0;
        }
        else if (hasCharacter)
        {
            if (p.Crouch != 0 && p.Crouched == 0 && p.Grounded != 0) { if (Resize(crouch)) p.Crouched = 1; }
            else if (p.Crouch == 0 && p.Crouched != 0 && Fits(stand)) { Resize(stand); p.Crouched = 0; }
        }
        // The eye's crouch: a critically damped spring (as quick as the old ease, ~0.3 s), so the view never jolts into
        // the move the way an exponential's first frame did.
        {
            const float w = 14f;
            float goal = p.Crouched != 0 ? 1 : 0, x = p.CrouchBlend - goal, v = p.CrouchBlendRate;
            float e = MathF.Exp(-w * dt), k = (v + w * x) * dt;
            p.CrouchBlend = goal + (x + k) * e;
            p.CrouchBlendRate = (v - w * k) * e;
            if (MathF.Abs(p.CrouchBlend - goal) < 1e-4f && MathF.Abs(p.CrouchBlendRate) < 1e-3f) { p.CrouchBlend = goal; p.CrouchBlendRate = 0; }
        }
        float eye = p.EyeHeight * (1 - p.CrouchBlend * (1 - Math.Clamp(p.CrouchHeight / Math.Max(.1f, p.SizeY), 0, 1)) * (p.CrouchHeight > 0 ? 1 : 0));
        bool sprint = p.Crouched == 0 && p.AimHeld == 0 && p.Sprint != 0;
        wish *= p.MoveSpeed * (sprint ? p.SprintMultiplier : 1) * (p.Crouched != 0 ? p.CrouchSpeedMultiplier : 1);
        p.WishVelocity = wish;
        Vector3 root = p.RootMotionVelocity; root.Y = 0;
        Vector3 target = sprint && wish.LengthSquared() > 1e-8f ? wish : Vector3.Lerp(wish, root, Math.Clamp(p.RootMotionWeight, 0, 1));
        Vector3 horizontal = Approach(new(p.Velocity.X, 0, p.Velocity.Z), target, dt,
            p.Grounded != 0 ? p.GroundAccelTime : p.AirAccelTime, p.Grounded != 0 ? p.GroundDecelTime : p.AirAccelTime);
        p.Velocity.X = horizontal.X; p.Velocity.Z = horizontal.Z;
        bool wasGrounded = p.Grounded != 0;
        p.Jumped = 0;
        p.SinceGrounded = wasGrounded ? 0 : p.SinceGrounded + dt;
        p.JumpBuffer = p.ReadInput != 0 && p.JumpDown != 0 ? p.JumpBufferTime : Math.Max(0, p.JumpBuffer - dt);
        if (p.ReadInput != 0 && p.JumpBuffer > 0 && (wasGrounded || p.SinceGrounded < p.CoyoteTime) && p.Velocity.Y <= .1f)
        {
            bool canStand = p.Crouched == 0 || (hasCharacter && Fits(stand));
            if (canStand)
            {
                if (p.Crouched != 0) { Resize(stand); p.Crouched = 0; }
                p.Velocity.Y = p.JumpSpeed; p.Jumped = 1; p.JumpBuffer = 0; p.SinceGrounded = 1000;
            }
        }
        p.Velocity.Y += p.Gravity * dt;
        Vector3 feet = p.Position - new Vector3(0, eye, 0);
        if (!Physics.IsActive) { p.Position += p.Velocity * dt; p.Grounded = 0; return; }
        if (!hasCharacter) PlayerCharacter.Create(radius, stand, feet);
        Vector3 from = Feet();
        int flags = Move(p.Velocity * dt, dt);
        if ((p.GroundAccelTime > 0 || p.GroundDecelTime > 0 || p.AirAccelTime > 0) && dt > 0)
        {
            Vector3 travelled = (Feet() - from) / dt; travelled.Y = 0;
            if (travelled.Length() < new Vector2(p.Velocity.X, p.Velocity.Z).Length()) { p.Velocity.X = travelled.X; p.Velocity.Z = travelled.Z; }
        }
        p.Grounded = (flags & 4) != 0 ? 1 : 0;
        if (p.Grounded == 0 && wasGrounded && p.Jumped == 0 && p.Velocity.Y <= 0)
        {
            Vector3 before = Feet();
            if ((Move(new(0, -.3f, 0), dt) & 4) != 0) p.Grounded = 1;
            else SetFeet(before);
        }
        if (p.Grounded != 0 && p.Velocity.Y < 0 || (flags & 2) != 0 && p.Velocity.Y > 0) p.Velocity.Y = 0;
        p.Yaw += PlayerCharacter.PlatformYawDelta;
        p.Position = Feet() + new Vector3(0, eye, 0);
        if (p.Position.Y < p.KillY) { SetFeet(p.RespawnFeet); p.Position = p.RespawnFeet + new Vector3(0, eye, 0); p.Velocity = Vector3.Zero; }
    }
}
