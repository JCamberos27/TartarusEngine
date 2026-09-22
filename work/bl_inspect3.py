# READ-ONLY inspection of the AKS74U source .blend.
# Reports: which actions exist, how the AK weapon object is attached to the arms rig,
# whether the weapon's own armature root is keyed, and the ik_hand_gun landmark.
# Nothing here mutates the file; run with:  blender.exe --background <blend> --python bl_inspect3.py
import bpy
import re


def action_fcurves(act):
    """Blender 5.x slotted actions: fcurves live in layer->strip->channelbag."""
    if act is None:
        return []
    try:
        legacy = list(act.fcurves)
        if legacy:
            return legacy
    except AttributeError:
        pass
    fcs = []
    for layer in getattr(act, "layers", []):
        for strip in getattr(layer, "strips", []):
            for cb in getattr(strip, "channelbags", []):
                fcs.extend(cb.fcurves)
    return fcs


print("=== ACTIONS (%d) ===" % len(bpy.data.actions))
for a in sorted(bpy.data.actions, key=lambda x: x.name):
    fr = a.frame_range
    print("  %-26s range=(%d..%d)  fcurves=%d" % (a.name, int(round(fr[0])), int(round(fr[1])), len(action_fcurves(a))))

print("=== OBJECTS ===")
for o in sorted(bpy.data.objects, key=lambda x: x.name):
    ad = o.animation_data
    act = ad.action.name if (ad and ad.action) else "-"
    print("  %-24s type=%-10s parent=%-16s parent_type=%-12s parent_bone=%-14s action=%s" % (
        o.name, o.type,
        o.parent.name if o.parent else "-",
        o.parent_type,
        o.parent_bone or "-",
        act))
    for c in o.constraints:
        print("        constraint %-18s target=%-12s subtarget=%s" % (
            c.type, c.target.name if c.target else "-", getattr(c, "subtarget", "") or "-"))
    for m in o.modifiers:
        tgt = getattr(m, "object", None)
        print("        modifier  %-18s object=%s" % (m.type, tgt.name if tgt else "-"))
    mw = o.matrix_world
    print("        world_loc=(%.4f, %.4f, %.4f)" % (mw.translation.x, mw.translation.y, mw.translation.z))

print("=== BONE LANDMARKS (world) ===")
for armname, bones in (("Armature", ("ik_hand_gun", "ik_hand_root", "hand_r", "head")),
                       ("AK", ("root",))):
    o = bpy.data.objects.get(armname)
    if not o:
        print("  %s: MISSING" % armname)
        continue
    for bn in bones:
        b = o.data.bones.get(bn)
        if not b:
            print("  %-10s %-14s MISSING" % (armname, bn))
            continue
        hw = o.matrix_world @ b.head_local
        chain = []
        p = b.parent
        while p is not None and len(chain) < 6:
            chain.append(p.name)
            p = p.parent
        print("  %-10s %-14s head_world=(%.4f, %.4f, %.4f)  parents=%s" % (
            armname, bn, hw.x, hw.y, hw.z, " -> ".join(chain) if chain else "-"))

print("=== KEYED BONES PER ACTION ===")
pat = re.compile(r'pose\.bones\["([^"]+)"\]')
for a in sorted(bpy.data.actions, key=lambda x: x.name):
    if not (a.name.startswith("A_W_") or a.name.startswith("A_FP_")):
        continue
    bones = set()
    objpaths = set()
    for fc in action_fcurves(a):
        m = pat.search(fc.data_path)
        if m:
            bones.add(m.group(1))
        else:
            objpaths.add(fc.data_path)
    kind = "W" if a.name.startswith("A_W_") else "FP"
    print("  [%s] %-26s n=%-3d bones=%s" % (kind, a.name, len(bones), ",".join(sorted(bones)) if bones else "-"))
    if objpaths:
        print("        NON-POSE fcurves: %s" % sorted(objpaths))

print("=== SCENE FRAME / ACTIVE ACTIONS ===")
sc = bpy.context.scene
print("  frame_current=%d  frame_start=%d  frame_end=%d  fps=%g" % (
    sc.frame_current, sc.frame_start, sc.frame_end, sc.render.fps))
for o in sorted(bpy.data.objects, key=lambda x: x.name):
    ad = o.animation_data
    if ad:
        print("  %-24s action=%s nla_tracks=%d" % (
            o.name, ad.action.name if ad.action else "-", len(ad.nla_tracks)))
print("DONE")
