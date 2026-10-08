using Tartarus;

namespace Tartarus.Gameplay;

public sealed class ScoreDigit : Script
{
    public ScoreTeam Team;
    public DigitPlace Place;
    [Range(0,100)] public float OnStrength=6;
    [Range(0,100)] public float OffStrength=.04f;
    static readonly byte[] Segments=[0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f];
    (GameObject Object,int Segment)[] children=[];
    int lastMask=-1;
    public override void OnReload() { lastMask=-1;Start(); }
    public override void Start() => children=gameObject.children.Select(c=>(c,c.name)).Where(x=>x.Item2.Length>=5 && x.Item2.StartsWith("Seg ") && x.Item2[4] is >= 'A' and <= 'G').Select(x=>(x.c,x.Item2[4]-'A')).ToArray();
    public override void Update() {
        var board=Scoreboard.Current;if(board==null)return;
        int score=Math.Clamp(Team==ScoreTeam.Home?board.Home:board.Away,0,99);
        int mask=Place==DigitPlace.Tens && score<10?0:Segments[Place==DigitPlace.Ones?score%10:score/10];
        if(lastMask==mask)return;lastMask=mask;
        foreach(var (child,segment) in children) if(child.IsValid) child.SetMaterialEmission((mask&(1<<segment))!=0?OnStrength:OffStrength);
    }
}
