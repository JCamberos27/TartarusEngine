namespace Tartarus;

/// <summary>Optional project-owned native adapter entry points. The engine assigns no game semantics to operation names.</summary>
public interface IProjectIntegration
{
    bool Invoke(string operation, nint frame, int size);
    string Request(string operation, string json);
    /// <summary>Project-wide state restored transactionally when a replacement assembly loads.</summary>
    string SaveState() => "{}";
    void LoadState(string json) { }
}
