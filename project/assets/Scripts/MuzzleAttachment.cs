using Tartarus;

namespace Tartarus.Gameplay;

public sealed class MuzzleAttachment : Attachment
{
    [SceneReference("Transform", true), Tooltip("Drag the muzzle transform from this attachment's hierarchy. Its local -Z axis is the bore direction.")]
    public string MuzzleTransform = "Muzzle";
    [SceneReference("Particle System", true), Tooltip("Drag the muzzle flash Particle System from this attachment's hierarchy. Its Texture supplies the flash asset.")]
    public string MuzzleFlash = "Muzzle";
    [SoundReferences, Tooltip("Drag audio assets into the firing sound slots. Variants alternate per shot. Empty uses the gun's layered audio.")]
    public string FiringSounds = "";
    [InspectorChoices("", "ak", "870"), Tooltip("Layered firing sound profile, used when Firing Sounds is empty.")]
    public string FiringSoundProfile = "";
    public Transform? Muzzle => Resolve(MuzzleTransform)?.transform;
    public GameObject? Flash => Resolve(MuzzleFlash);
}
