using Tartarus;

namespace Tartarus.Gameplay;

public enum ScoreTeam { Home, Away }
public enum DigitPlace { Ones, Tens }

public sealed class Scoreboard : Script
{
    [Range(0,999)] public int Home;
    [Range(0,999)] public int Away;
    internal static readonly List<Scoreboard> Boards=[];
    internal static Scoreboard? Current => Boards.FirstOrDefault(b=>b.enabled && b.gameObject.IsValid && b.gameObject.activeInHierarchy);
    public override void Awake() => Boards.Add(this);
    public override void OnReload() => Boards.Add(this);
    public override void OnDestroy() { Boards.Remove(this); if(Boards.Count==0) BasketState.Reset(); }
    internal void Add(ScoreTeam team,int points) {if(team==ScoreTeam.Home) Home+=points;else Away+=points;}
}
