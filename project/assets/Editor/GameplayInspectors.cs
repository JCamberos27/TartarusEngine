using Tartarus;
using Tartarus.Editor;

namespace Tartarus.EditorTools;

/// <summary>Shared Inspector layout: pages, searchable fields and grouped foldouts.
/// Every native property keeps its C++ widget, validation, units and asset picker.</summary>
public abstract class GameplayInspector : Tartarus.Editor.Editor
{
    int page;
    string search = "";
    protected abstract string[] Pages { get; }
    protected abstract string Summary { get; }
    protected abstract string PageFor(SerializedProperty property);
    public override void OnInspectorGUI()
    {
        page = EditorGUILayout.Tabs(page, Pages);
        EditorGUILayout.HelpBox(Summary);
        EditorGUILayout.TextField("Find setting", ref search);
        EditorGUILayout.Separator();
        var all = serializedObject.Properties;
        var visible = all.Where(p => search.Length > 0
            ? (p.DisplayName + " " + p.Group + " " + p.Tooltip).Contains(search, StringComparison.OrdinalIgnoreCase)
            : PageFor(p) == Pages[page]);
        int count = 0;
        foreach (var group in visible.GroupBy(p => string.IsNullOrWhiteSpace(p.Group) ? Pages[page] : p.Group)) {
            var fields = group.Where(p => p.IsVisible).ToArray();
            if (fields.Length == 0) continue;
            count += fields.Length;
            if (!EditorGUILayout.Foldout($"{group.Key} ({fields.Length})###{group.First().Name}", true)) continue;
            try { foreach (var property in fields) EditorGUILayout.PropertyField(property); }
            finally { EditorGUILayout.EndFoldout(); }
        }
        if (count == 0) EditorGUILayout.Label("No matching settings.");
        EditorGUILayout.Separator();
        EditorGUILayout.Label($"{all.Count} authored settings • changes use global undo / redo");
        serializedObject.ApplyModifiedProperties();
    }
    protected static bool Has(SerializedProperty p, params string[] terms) => terms.Any(t =>
        (p.DisplayName + " " + p.Group).Contains(t, StringComparison.OrdinalIgnoreCase));
}

[CustomEditor("First Person Body")]
public sealed class FpsBodyInspector : GameplayInspector
{
    protected override string[] Pages => ["Overview", "Rig & Camera", "Arms & Aim", "Locomotion", "Turning & Transitions", "Foot IK", "NPC", "Advanced"];
    protected override string Summary => "FPS body: configure the rig and camera first, then arm posing, movement and foot placement. Advanced tuning stays on separate pages. Search checks every page.";
    protected override string PageFor(SerializedProperty p)
    {
        if (Has(p, "NPC")) return "NPC";
        if (Has(p, "Foot", "Stride", "Pinning")) return "Foot IK";
        if (Has(p, "Turning", "Turn ", "Start", "Stop", "Transition")) return "Turning & Transitions";
        if (Has(p, "Camera", "Head Bone", "Hidden", "Eye Offset")) return "Rig & Camera";
        if (Has(p, "Arms", "Spine", "Hand", "Elbow", "Shoulder", "Aim")) return "Arms & Aim";
        if (Has(p, "Locomotion", "Speed", "Play Rate", "Blend", "Root Motion", "Responsive", "Responsiveness")) return "Locomotion";
        return p.Group.Length == 0 || Has(p, "Enabled", "Controller", "Model", "Rig") ? "Overview" : "Advanced";
    }
}

[CustomEditor("First Person Controller")]
public sealed class PlayerInspector : GameplayInspector
{
    protected override string[] Pages => ["Overview", "Movement", "Jump & Crouch", "View & Input", "Weapons", "Gravity Gun", "Advanced"];
    protected override string Summary => "C# PlayerController consumes these authored movement settings. Weapons use the AK and 870 prefab slots; view and input tuning are separate from locomotion.";
    protected override string PageFor(SerializedProperty p)
    {
        if (Has(p, "Grab", "Assist", "Scroll Turn", "Held", "Throw", "Gravity Gun")) return "Gravity Gun";
        if (Has(p, "Weapon", "Animation Set", "View Model", "Camera Bone")) return "Weapons";
        if (Has(p, "Jump", "Coyote", "Crouch", "Ground Snap")) return "Jump & Crouch";
        if (Has(p, "Look", "Yaw", "Pitch", "Input", "Sensitivity", "Eye", "FOV", "Field of View", "Invert")) return "View & Input";
        if (Has(p, "Speed", "Sprint", "Gravity", "Accel", "Decel", "Air", "Ground", "Root Motion")) return "Movement";
        return p.Group.Length == 0 ? "Overview" : "Advanced";
    }
}

[CustomEditor("Weapon Definition")]
public sealed class WeaponInspector : GameplayInspector
{
    protected override string[] Pages => ["Overview", "Muzzle Light", "Flame", "Flash", "Smoke"];
    protected override string Summary => "This weapon owns its muzzle effects. Animation Set supplies animation, gameplay, recoil and camera profiles. Save prefab changes before starting Play.";
    protected override string PageFor(SerializedProperty property) => property.Group.Length > 0 ? property.Group : "Overview";
}

[CustomEditor("Particle System")]
public sealed class ParticleInspector : GameplayInspector
{
    protected override string[] Pages => ["Emission", "Shape", "Lifetime", "Motion", "Collision", "Rendering"];
    protected override string Summary => "Particle modules preview in the editor. Use Restart or Emit Burst to preview one-shots. Lifetime curves open in separate curve windows; world collision runs during Play.";
    protected override string PageFor(SerializedProperty property) => property.Group.Length > 0 ? property.Group : "Emission";
}

[CustomEditor("Tartarus.Gameplay.Bob")]
public sealed class BobInspector : GameplayInspector
{
    protected override string[] Pages => ["Overview"];
    protected override string Summary => "Bobs this object around its starting position. Height is in metres; Speed controls the sine wave rate. Runtime origin and elapsed time stay hidden.";
    protected override string PageFor(SerializedProperty property) => "Overview";
}

/// <summary>Every other attachable C# script gets searchable, organized serialized fields.</summary>
[CustomEditor("*")]
public sealed class ScriptInspector : GameplayInspector
{
    protected override string[] Pages => ["Overview"];
    protected override string Summary => "Authored C# fields. Add [Header], [Tooltip] and [Range] to organize your script, or implement a dedicated [CustomEditor] in assets/Editor.";
    protected override string PageFor(SerializedProperty property) => "Overview";
}
