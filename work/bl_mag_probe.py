# READ-ONLY. Where is the spare/second magazine the reload animation puts in the left hand?
#
# The .blend has exactly one weapon object (`aks74u`), so the spare magazine cannot be a
# separate mesh - it is either a mesh island inside `aks74u` weighted to one of the AK armature's
# bones, or something excluded from the export entirely. This reports the facts that decide it:
#   * the AK armature's bone names, and `aks74u`'s vertex groups with per-group vertex counts
#   * any object whose name looks like a magazine/spare, plus every hide_render/hide_viewport flag
#     and any fcurve that keys one
#   * for each A_W_* action: which AK bones it keys, and the evaluated WORLD position of each
#     bone at the action's first / middle / last frame
#   * the evaluated world-space bounding box of the vertices weighted to each bone, at those
#     same frames - i.e. "does the spare mag move into the left hand, and where is it?"
#
# Nothing is saved: actions / NLA / pose_position / frame are restored in memory, no .save().
#
#   blender --background "<blend>" --python work/bl_mag_probe.py
import bpy

ARMS, WEAP = "Armature", "AK"
HINTS = ("mag", "spare", "second", "reload", "clip")


def action_fcurves(act):
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


wep = bpy.data.objects.get(WEAP)
mesh_obj = bpy.data.objects.get("aks74u")

print("\n=== AK ARMATURE BONES ===")
ak_bones = []
if wep and wep.type == "ARMATURE":
    ak_bones = [b.name for b in wep.data.bones]
    for b in wep.data.bones:
        print("  %-22s head=%s  connected=%s" % (
            b.name, tuple(round(v, 4) for v in b.head_local), b.use_connect))
print("  count=%d" % len(ak_bones))

print("\n=== aks74u VERTEX GROUPS (bone -> vertex count) ===")
group_stats = []
if mesh_obj and mesh_obj.type == "MESH":
    me = mesh_obj.data
    counts = [0] * len(mesh_obj.vertex_groups)
    contrib = [0] * len(mesh_obj.vertex_groups)
    for v in me.vertices:
        for g in v.groups:
            counts[g.group] += 1
            if g.weight > 1e-4:
                contrib[g.group] += 1
    for i, g in enumerate(mesh_obj.vertex_groups):
        group_stats.append((g.name, counts[i], contrib[i]))
        print("  %-22s verts_assigned=%-7d verts_weighted=%d" % (g.name, counts[i], contrib[i]))
    print("  mesh verts=%d polys=%d  groups=%d" % (len(me.vertices), len(me.polygons),
                                                   len(mesh_obj.vertex_groups)))

print("\n=== OBJECTS WHOSE NAME LOOKS LIKE A MAGAZINE ===")
hits = [o for o in bpy.data.objects if any(h in o.name.lower() for h in HINTS)]
if not hits:
    print("  none - no separately-named magazine/spare object exists")
for o in hits:
    print("  %-30s type=%-8s verts=%d parent=%s hide_vp=%s hide_render=%s" % (
        o.name, o.type, len(o.data.vertices) if o.type == "MESH" else 0,
        o.parent.name if o.parent else "-", o.hide_viewport, o.hide_render))

print("\n=== HIDDEN OBJECTS (static) ===")
for o in bpy.data.objects:
    if o.hide_render or o.hide_viewport:
        print("  %-30s hide_viewport=%s hide_render=%s  parent=%s" % (
            o.name, o.hide_viewport, o.hide_render, o.parent.name if o.parent else "-"))

print("\n=== FCURVES THAT KEY VISIBILITY ===")
found = False
for o in bpy.data.objects:
    ad = o.animation_data
    if not ad:
        continue
    for fc in action_fcurves(ad.action):
        if "hide" in fc.data_path:
            found = True
            vals = [k.co[1] for k in fc.keyframe_points]
            print("  %-28s %s idx=%d keys=%d range=%s..%s" % (
                o.name, fc.data_path, fc.array_index, len(vals),
                min(vals) if vals else "-", max(vals) if vals else "-"))
if not found:
    print("  none - nothing animates object visibility")

# --- evaluated motion per AK bone under each weapon action -------------------------------
saved = {
    "wep_action": wep.animation_data.action if wep.animation_data and wep.animation_data else None,
    "wep_pose": wep.data.pose_position,
    "frame": bpy.context.scene.frame_current,
}
saved_nla = [(t, t.mute) for t in wep.animation_data.nla_tracks] if wep.animation_data else []

scn = bpy.context.scene
print("\n=== WHICH BONES EACH A_W_* ACTION KEYS, AND WHERE THEY GO (world, metres) ===")
for act in sorted(bpy.data.actions, key=lambda a: a.name):
    if not act.name.startswith("A_W_"):
        continue
    keyed = sorted({fc.data_path for fc in action_fcurves(act)})
    bone_keys = sorted({p.split('"')[1] for p in keyed if '"' in p})
    fs, fe = int(round(act.frame_range[0])), int(round(act.frame_range[1]))
    print("\n  -- %-24s frames=%d..%d  bones_keyed=%s" % (act.name, fs, fe, bone_keys or "-"))

    wep.animation_data.action = act
    for t, _ in saved_nla:
        t.mute = True
    wep.data.pose_position = "POSE"
    samples = [fs, (fs + fe) // 2, fe]
    header = "     %-18s" % "bone"
    for f in samples:
        header += " f%-6d" % f
    print(header)
    for b in ak_bones:
        row = "     %-18s" % b
        pb = wep.pose.bones.get(b)
        if pb is None:
            print(row + "  (missing)")
            continue
        for f in samples:
            scn.frame_set(f)
            bpy.context.view_layer.update()
            loc = (wep.matrix_world @ pb.matrix).translation
            row += " (%6.3f,%6.3f,%6.3f)" % (loc.x, loc.y, loc.z)
        print(row)

# --- where does the geometry weighted to each bone actually sit? -------------------------
print("\n=== EVALUATED WORLD BBOX of the verts weighted to each group (reload frames) ===")
if mesh_obj and group_stats:
    dg = bpy.context.evaluated_depsgraph_get()
    act = bpy.data.actions.get("A_W_Tac_Reload")
    if act is not None:
        wep.animation_data.action = act
        wep.data.pose_position = "POSE"
        fs, fe = int(round(act.frame_range[0])), int(round(act.frame_range[1]))
        for f in (fs, (fs + fe) // 2, fe):
            scn.frame_set(f)
            bpy.context.view_layer.update()
            dg.update()
            ev = mesh_obj.evaluated_get(dg)
            evme = ev.to_mesh()
            mw = ev.matrix_world
            print("  frame %d" % f)
            for gname, cnt, _w in group_stats:
                gi = mesh_obj.vertex_groups.find(gname)
                if gi < 0 or cnt == 0:
                    continue
                pts = []
                for v in evme.vertices:
                    for g in v.groups:
                        if g.group == gi and g.weight > 1e-4:
                            pts.append(mw @ v.co)
                            break
                if not pts:
                    continue
                xs = [p.x for p in pts]; ys = [p.y for p in pts]; zs = [p.z for p in pts]
                cx = sum(xs) / len(xs); cy = sum(ys) / len(ys); cz = sum(zs) / len(zs)
                print("     %-14s n=%-5d centroid=(%7.3f,%7.3f,%7.3f)  min=(%7.3f,%7.3f,%7.3f)"
                      % (gname, len(pts), cx, cy, cz, min(xs), min(ys), min(zs)))
            ev.to_mesh_clear()

wep.animation_data.action = saved["wep_action"]
wep.data.pose_position = saved["wep_pose"]
for t, m in saved_nla:
    t.mute = m
scn.frame_set(saved["frame"])
print("\nRESTORED weapon=%s frame=%d" % (
    saved["wep_action"].name if saved["wep_action"] else None, saved["frame"]))
print("DONE")
