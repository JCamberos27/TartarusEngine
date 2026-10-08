using Tartarus;

namespace Tartarus.Gameplay;

/// <summary>Base for interchangeable children of a weapon prefab. One child per category is active.</summary>
public abstract class Attachment : Script
{
    // Paths are relative to this attachment, so prefab instances never store scene entity IDs.
    protected GameObject? Resolve(string path)
    {
        GameObject current = gameObject;
        foreach (string part in path.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            if (part == ".") continue;
            if (part == "..") { if (current.parent is not { } parent) return null; current = parent; continue; }
            var child = current.children.FirstOrDefault(c => c.name == part, GameObject.Invalid);
            if (child == GameObject.Invalid) return null;
            current = child;
        }
        return current;
    }
}
