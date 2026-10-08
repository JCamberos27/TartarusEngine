using Tartarus;

namespace Tartarus.Gameplay;

public sealed class OpticAttachment : Attachment
{
    [SceneReference("Transform", true), Tooltip("Drag the aim point from this optic's hierarchy. Its local -Z axis points along the sight line.")]
    public string AimPoint = "Aim Point";
    [Tooltip("Marks the iron-sight aim point as the reference for the weapon's existing ADS animation.")]
    public bool DefaultOptic = false;
    [Range(0, 1), Tooltip("Seconds to move between aim points when changing optics while aiming.")]
    public float AimBlendTime = .2f;
    public Transform? Aim => Resolve(AimPoint)?.transform;
}
