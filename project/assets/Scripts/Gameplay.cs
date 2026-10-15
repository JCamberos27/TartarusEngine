using Tartarus;

namespace Tartarus.Gameplay;

public sealed class Gameplay : IProjectIntegration
{
    readonly PlayerController player = new();
    readonly WeaponController weapon = new();
    public void Player(ref PlayerFrame frame) => player.Update(ref frame);
    public void Weapon(ref WeaponFrame frame) => weapon.Update(ref frame);
    public void Shot(ref ShotFrame frame) => weapon.FireShot(ref frame);
    public unsafe bool Invoke(string operation,nint frame,int size) {
        if(frame==0)return false;
        switch(operation) {
        case "player" when size==sizeof(PlayerFrame):Player(ref *(PlayerFrame*)frame);return true;
        case "weapon" when size==sizeof(WeaponFrame):Weapon(ref *(WeaponFrame*)frame);return true;
        case "shot" when size==sizeof(ShotFrame):Shot(ref *(ShotFrame*)frame);return true;
        default:return GameIntegration.Invoke(operation,frame,size);
        }
    }
    public string SaveState()=>System.Text.Json.JsonSerializer.Serialize(new {health=HealthRules.Save()});
    public void LoadState(string json) {
        using var doc=System.Text.Json.JsonDocument.Parse(json);
        if(doc.RootElement.TryGetProperty("health",out var health))HealthRules.Load(health.GetString()!);
    }
    public string Request(string operation,string json) => GameIntegration.Request(operation,json);
}
