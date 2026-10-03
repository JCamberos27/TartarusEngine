"""Contact analysis of the first-person weapon animation clips -> tools/audio/sync_map.json.

Two stages, one file.

  1. dump   (runs INSIDE headless Blender -- a separate process, never the user's open session or files):
       blender.exe -b --python tools/audio/clip_contacts.py -- dump <dump_dir>
     Imports every FP / Weapon FBX of both guns into a factory-empty scene and samples, for every frame, the head of
     the relevant bones: in world space and relative to the gun's own root (gun sway / recoil cancelled, so only the
     part's own motion remains).

  2. analyze (plain python + numpy):
       uv run --with numpy python tools/audio/clip_contacts.py analyze <dump_dir> tools/audio/sync_map.json

Detection: per-part speed profiles (magazine, mag_release, bolt, pump, shell, hands). A "segment" is a run of motion;
its start is a release / grab / whoosh, its stop is an impact (mag seated, bolt slam, pump stop, hand arriving). Rules
below pair those detections with sound elements and assert the order of each reload sequence (release < out < in <
bolt back < bolt release; pump back < shell < pump forward ...). Frames are 0-based clip frames at 60 fps
(Blender frame - 1); "time" = frame / clip length in frames (the export_manifest frameEnd).
"""
import json
import os
import sys

GUN_DIR = {"ak": "AKS74U", "870": "Remington870"}
PREFIX = {"ak": "AKS-74U", "870": "Remington870"}
CLIPS = {
    "ak": ["Draw", "Holster", "Fire", "Tac_Reload", "Empty_Reload", "Mag_Check", "Inspect", "Melee", "Regrip"],
    "870": ["Draw", "Holster", "Fire", "Pump", "Reload_Start", "Reload_Start_Empty", "Reload_Loop",
            "Reload_Loop_End", "Reload_End", "Mag_Check", "Inspect", "Melee", "Regrip"],
}
FP_BONES = ["ik_hand_gun", "hand_l", "hand_r", "lowerarm_l", "lowerarm_r"] + \
    [f"{f}_03_{s}" for s in "lr" for f in ("index", "middle", "ring", "pinky", "thumb")]
PROJECT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "project"))
MAG_CLEAR = 4.0   # AK magazine travel (weapon-space units) at which it has cleared the well
LEAD_FRAMES = 0   # frames are CONTACT frames; the slide lead-in of a pump/bolt one-shot lives in the file (manifest anchor_ms)


# ------------------------------------------------------------------------------------------ stage 1: dump (Blender)
def dump(out_dir):
    import bpy
    os.makedirs(out_dir, exist_ok=True)
    for gun, folder in GUN_DIR.items():
        for kind, sub, tag in (("fp", "FirstPerson", "A_FP"), ("w", "Weapon", "A_W")):
            for clip in CLIPS[gun]:
                path = os.path.join(PROJECT, "assets", "Weapons", folder, sub, f"{PREFIX[gun]}_{tag}_{clip}.fbx")
                if not os.path.exists(path):
                    continue
                bpy.ops.wm.read_factory_settings(use_empty=True)
                bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=False)
                scn = bpy.context.scene
                arms = [o for o in scn.objects if o.type == 'ARMATURE' and o.animation_data and o.animation_data.action]
                if not arms:
                    continue
                arm = arms[0]
                a = arm.animation_data.action
                f0, f1 = int(round(a.frame_range[0])), int(round(a.frame_range[1]))
                names = FP_BONES if kind == "fp" else [b.name for b in arm.data.bones]
                names = [n for n in names if n in arm.pose.bones]
                root_name = "ik_hand_gun" if kind == "fp" else ("root" if "root" in arm.pose.bones else "Main")
                world = {n: {"head": [], "rel_head": []} for n in names}
                for f in range(f0, f1 + 1):
                    scn.frame_set(f)
                    inv = (arm.matrix_world @ arm.pose.bones[root_name].matrix).inverted()
                    for n in names:
                        h = arm.matrix_world @ arm.pose.bones[n].head
                        world[n]["head"].append([round(v, 5) for v in h])
                        world[n]["rel_head"].append([round(v, 5) for v in (inv @ h)])
                out = {"gun": gun, "clip": clip, "kind": kind, "frames": f1 - f0 + 1, "bones": world}
                with open(os.path.join(out_dir, f"{gun}_{kind}_{clip}.json"), "w") as fh:
                    json.dump(out, fh)
                print("dumped", gun, kind, clip, f1 - f0 + 1)


# ------------------------------------------------------------------------------------------ stage 2: analyze
class Clip:
    def __init__(self, dump_dir, gun, kind, clip):
        import numpy as np
        self.np = np
        path = os.path.join(dump_dir, f"{gun}_{kind}_{clip}.json")
        self.d = json.load(open(path)) if os.path.exists(path) else None
        self.length = (self.d["frames"] - 1) if self.d else 0   # clip length in frames (export_manifest frameEnd)

    def __bool__(self):
        return self.d is not None

    def pos(self, bone, space="rel_head"):
        return self.np.array(self.d["bones"][bone][space], float)

    def rows(self, bone, space="rel_head", tele=True):
        """row i = distance moved between frame i and i+1; teleports (bone swaps / visibility pops) are zeroed."""
        np = self.np
        v = np.linalg.norm(np.diff(self.pos(bone, space), axis=0), axis=1)
        if tele and (v > 0).any():
            v = np.where(v > 6.0 * np.percentile(v[v > 0], 90), 0.0, v)
        return v

    def segs(self, bone, space="rel_head", frac=0.1, gap=3, tele=True):
        """moving segments (start, stop, peak_frame, peak_fraction): pose changes from `start`, settles at `stop`."""
        np = self.np
        v = self.rows(bone, space, tele)
        if v.size == 0 or v.max() <= 0:
            return []
        mv = v > frac * v.max()
        out, i, n = [], 0, len(v)
        while i < n:
            if mv[i]:
                j, last = i, i
                while j < n and (mv[j] or j - last <= gap):
                    if mv[j]:
                        last = j
                    j += 1
                seg = v[i:last + 1]
                out.append((i, last + 1, i + int(np.argmax(seg)), float(seg.max() / v.max())))
                i = last + 1
            else:
                i += 1
        return out


def analyze(dump_dir, out_path):
    result = {}
    problems = []
    for gun in ("ak", "870"):
        for clip in CLIPS[gun]:
            fp, w = Clip(dump_dir, gun, "fp", clip), Clip(dump_dir, gun, "w", clip)
            if not fp:
                continue
            L = fp.length
            ev = []      # (key, frame, source, confidence)
            np = fp.np

            def add(elem, frame, src, conf="high"):
                ev.append((f"snd.{gun}.{elem}", int(max(0, min(L, frame))), src, conf))

            # ---------------------------------------------------------------- AK: magazine, bolt
            if gun == "ak":
                if clip in ("Tac_Reload", "Empty_Reload", "Mag_Check") and w:
                    seat = w.pos("magazine")[0]
                    r = w.segs("mag_release", frac=0.3, tele=False)
                    mag_rows = w.rows("magazine")
                    if not r:
                        problems.append(f"{gun}/{clip}: no mag_release motion")
                    else:
                        rel = r[0][0]
                        add("mag_release", rel, "mag_release bone starts moving (button press)")
                        start = next((i for i in range(rel, len(mag_rows))
                                      if mag_rows[i] > 0.1 * mag_rows[rel:].max()), rel)
                        # the mag has left the well once it is MAG_CLEAR units from its seat
                        dist = np.linalg.norm(w.pos("magazine") - seat, axis=1)
                        out_f = next((f for f in range(start, len(dist)) if dist[f] > MAG_CLEAR), start)
                        add("mag_out", out_f, f"magazine is {MAG_CLEAR} units clear of the well")
                        seated = None
                        for f in range(out_f + 10, w.d["frames"]):
                            for b in ("magazine", "mag2"):
                                if np.linalg.norm(w.pos(b)[f] - seat) < 1e-3 and np.linalg.norm(w.pos(b)[f - 3] - seat) > 0.3:
                                    seated = f
                                    break
                            if seated is not None:
                                break
                        if seated is None:
                            problems.append(f"{gun}/{clip}: magazine never reseats")
                        else:
                            add("mag_in", seated, "magazine reaches the seat (stop)")
                if clip == "Empty_Reload" and w:
                    s = w.segs("bolt", frac=0.2, tele=False)
                    if len(s) >= 2:
                        add("bolt_back", s[0][1] - LEAD_FRAMES, "bolt reaches its rear stop")
                        add("bolt_release", s[1][1] - LEAD_FRAMES, "bolt slams forward onto the chamber")
                if clip == "Inspect" and w:
                    s = w.segs("bolt", frac=0.2, tele=False)   # chamber check: eased pull, then a fast return
                    if len(s) >= 2:
                        add("bolt_back", s[0][0], "bolt eases back (chamber check)")
                        add("bolt_release", s[1][1] - LEAD_FRAMES, "bolt returns fast onto the chamber")
                if clip == "Tac_Reload":
                    seated = [e[1] for e in ev if e[0].endswith(".mag_in")]
                    seg = [s for s in fp.segs("hand_l", frac=0.1) if seated and s[0] > seated[0] + 5]
                    if seg:
                        add("mag_tap", seg[0][0], "left hand taps / tugs the seated magazine", "medium")
                if clip == "Fire" and w:
                    add("fire", w.segs("bolt", frac=0.2, tele=False)[0][0], "bolt cycles; shot at the first frame")
            # ---------------------------------------------------------------- 870: pump, shells
            else:
                if clip in ("Pump", "Mag_Check", "Inspect", "Reload_Start_Empty") and w:
                    p = []
                    for s in w.segs("Pump", frac=0.12, tele=False):
                        if p and s[0] - p[-1][1] <= 3:
                            p[-1] = (p[-1][0], s[1], 0, 0.0)     # one stroke split by a hesitation
                        else:
                            p.append(s)
                    if len(p) >= 2:
                        add("pump_back", p[0][1] - LEAD_FRAMES, "forend reaches its rear stop")
                        add("pump_fwd", p[-1][1] - LEAD_FRAMES, "forend slams forward")
                    else:
                        problems.append(f"870/{clip}: pump strokes {len(p)}")
                def shell_contact(seg):
                    """first frame after the shell's peak speed that it has slowed below 40 % = it met the port."""
                    v = w.rows("Shell")
                    a, b, pk, _ = seg
                    return next((f for f in range(pk, b) if v[f] < 0.4 * v[pk]), b), b
                if clip == "Reload_Start_Empty" and w:
                    sh = [s for s in w.segs("Shell", frac=0.05) if s[1] - s[0] > 8]
                    if sh:
                        add("shell_load_chamber", shell_contact(sh[0])[0], "first shell meets the open action port")
                if clip in ("Reload_Loop", "Reload_Loop_End") and w:
                    sh = [s for s in w.segs("Shell", frac=0.05) if s[1] - s[0] > 8]
                    if sh:
                        c, end = shell_contact(sh[0])
                        add("shell_insert", c, "shell meets the loading port")
                        add("shell_insert", end - 2, "shell slides home past the latch (soft second click)", "medium")
                if clip == "Fire":
                    add("fire", 0, "shot at the first frame")

            # ---------------------------------------------------------------- strikes / draw / holster (hands)
            if clip == "Melee":
                hl = fp.segs("hand_l", "head", frac=0.2)
                add("melee_swing", 0, "strike starts", "medium")
                if hl:
                    add("melee_hit", hl[0][1] - 1, "left hand arrives = contact", "medium")
                later = [s for s in fp.segs("hand_r", "head", frac=0.2) if s[0] > 15]
                if later:
                    add("cloth", later[0][0], "recovery movement", "medium")
            if clip in ("Draw", "Holster"):
                hl = fp.segs("hand_l", "head", frac=0.2)
                hr = fp.segs("hand_r", "head", frac=0.2)
                # contact = the gun arriving: the draw/holster one-shots carry their thud at manifest anchor_ms
                # (draw ~360 ms / holster ~300 ms of cloth lead-in), so the file starts before this frame
                arrive = (hl[0][1] if hl else (hr[0][1] if hr else 0))
                add(clip.lower(), arrive, "gun settles in the hands / holster seats (left hand arrives)", "medium")

            # ---------------------------------------------------------------- hands: cloth + handle (medium)
            if clip not in ("Fire", "Draw", "Holster", "Melee", "Pump"):
                taken = [e[1] for e in ev]
                last_cloth = -99
                for hand, space in (("hand_l", "rel_head"), ("hand_r", "head")):
                    for (a, b, pk, pf) in fp.segs(hand, space, frac=0.25):
                        if pf < 0.35:
                            continue
                        if a - last_cloth >= 10 and all(abs(a - t) > 4 for t in taken):
                            add("cloth", a, f"{hand} starts moving (peak {pf:.2f})", "medium")
                            last_cloth = a
                            taken.append(a)
                        if b - a <= 45 and pf >= 0.5 and all(abs(b - t) > 4 for t in taken):
                            add("handle", b, f"{hand} arrives / settles", "medium")
                            taken.append(b)
            ev.sort(key=lambda e: (e[1], e[0]))

            # ---------------------------------------------------------------- sequence sanity checks
            by = {}
            for k, f, *_ in ev:
                by.setdefault(k.split(".", 2)[2], []).append(f)
            order = {
                ("ak", "Tac_Reload"): ["mag_release", "mag_out", "mag_in"],
                ("ak", "Empty_Reload"): ["mag_release", "mag_out", "mag_in", "bolt_back", "bolt_release"],
                ("ak", "Mag_Check"): ["mag_release", "mag_out", "mag_in"],
                ("ak", "Inspect"): ["bolt_back", "bolt_release"],
                ("870", "Pump"): ["pump_back", "pump_fwd"],
                ("870", "Reload_Start_Empty"): ["pump_back", "shell_load_chamber", "pump_fwd"],
                ("870", "Mag_Check"): ["pump_back", "pump_fwd"],
                ("870", "Inspect"): ["pump_back", "pump_fwd"],
            }.get((gun, clip), [])
            seq = [by[e][0] for e in order if e in by]
            if len(seq) != len(order) or seq != sorted(seq):
                problems.append(f"{gun}/{clip}: sequence broken, expected {order} got {seq}")

            result[f"{gun}/{clip}"] = [
                {"key": k, "frame": f, "time": round(f / L, 4), "src": src, "conf": conf} for k, f, src, conf in ev]

    meta = {"fps": 60, "frame_base": 0,
            "time": "frame / clip length in frames (the export_manifest frameEnd)",
            "note": "frame = the CONTACT frame of the event (impact / press / grab). Play the one-shot at "
                    "(frame / 60 s - anchor_ms / 1000) where anchor_ms comes from audio_manifest.json: pump and bolt "
                    "one-shots carry their slide lead-in before the contact transient, so the file starts slightly "
                    "BEFORE the frame (clamp at clip start). conf: high = driven by part motion "
                    "(mag/bolt/pump/shell), medium = derived from hand motion."}
    with open(out_path, "w") as fh:
        json.dump({"_meta": meta, **result}, fh, indent=1)
    for k, v in result.items():
        print(k, " ".join(f"{e['key'].split('.', 2)[2]}@{e['frame']}" for e in v))
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  ", p)
        raise SystemExit(1)


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if args[0] == "dump":
        dump(args[1])
    else:
        analyze(args[1], args[2])
