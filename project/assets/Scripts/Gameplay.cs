using Tartarus;

namespace Tartarus.Gameplay;

public sealed class Gameplay : IGameplay
{
    readonly PlayerController player = new();
    readonly WeaponController weapon = new();
    public void Player(ref PlayerFrame frame) => player.Update(ref frame);
    public void Weapon(ref WeaponFrame frame) => weapon.Update(ref frame);
    public void Shot(ref ShotFrame frame) => weapon.FireShot(ref frame);
}
