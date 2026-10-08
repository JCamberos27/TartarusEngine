# The model library's "kits" ship their moving parts laid out beside the body (a lamp's shade on the
# floor next to its stand, the fridge door flat behind the fridge, the toilet seat and lid at its feet). This puts each
# part where it belongs and exports <Stem>_Assembled.fbx next to the original, with a .meta copied from the original's
# (same materials, scale and pivot, new guid). The originals stay for anyone who wants the loose parts.
#
#   python tools/assets/assemble_kits.py <repo> [--blender <blender.exe>] [--exe <TartarusEngine.exe>]
# runs itself inside Blender to build the files, then re-centres each one's pivot (bottom centre) from the engine's
# own --model-report, so the exe must be built.
import glob
import json
import math
import os
import secrets
import subprocess
import sys

try:
    import bpy
    from mathutils import Matrix, Vector
except ImportError:
    bpy = None


def world_verts(o):
    return [o.matrix_world @ v.co for v in o.data.vertices]


def bounds(o):
    pts = world_verts(o)
    return Vector([min(p[i] for p in pts) for i in range(3)]), Vector([max(p[i] for p in pts) for i in range(3)])


def rotate_about(o, axis, deg, pivot):
    o.matrix_world = (Matrix.Translation(pivot) @ Matrix.Rotation(math.radians(deg), 4, axis)
                      @ Matrix.Translation(-pivot) @ o.matrix_world)
    bpy.context.view_layer.update()


def move(o, delta):
    o.matrix_world = Matrix.Translation(delta) @ o.matrix_world
    bpy.context.view_layer.update()


def stand_shade(body, shade, sink=0.55):
    """A lampshade lying on its side next to the stand: stand it up, wide end down, over the stand's top."""
    lo, hi = bounds(shade)
    rotate_about(shade, 'X', 90, (lo + hi) / 2)
    lo, hi = bounds(shade)
    # wide end down: compare the spread of the bottom and top tenths
    h = hi.z - lo.z
    c = (lo + hi) / 2

    def spread(zlo, zhi):
        r = [((p.x - c.x) ** 2 + (p.y - c.y) ** 2) ** 0.5 for p in world_verts(shade) if zlo <= p.z <= zhi]
        return max(r) if r else 0.0
    if spread(hi.z - 0.1 * h, hi.z) > spread(lo.z, lo.z + 0.1 * h) + 1e-3:
        rotate_about(shade, 'X', 180, c)
    blo, bhi = bounds(body)
    lo, hi = bounds(shade)
    target = Vector(((blo.x + bhi.x) / 2, (blo.y + bhi.y) / 2, bhi.z - sink * (hi.z - lo.z)))
    move(shade, target - Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z)))


def top_of(body, front_of_y=None):
    """Highest vertex z, optionally only over the front part (y below front_of_y; the engine's +Z front is -Y here)."""
    return max(p.z for p in world_verts(body) if front_of_y is None or p.y < front_of_y)


def fridge(objs):
    body, door = objs['Fridge'], objs['Fridge_door']
    blo, bhi = bounds(body)
    # the crisper boxes sit behind the fridge on the floor: into the bottom of it
    for name in ('Fridge_food_box_A', 'Fridge_food_box_B'):
        lo, hi = bounds(objs[name])
        move(objs[name], Vector((0.0, blo.y + 0.05 - lo.y, blo.z + 0.06 - lo.z)))
    lo, hi = bounds(door)
    # flat on its back behind the fridge -> upright across the front, shelves facing in
    rotate_about(door, 'X', 90, (lo + hi) / 2)
    lo, hi = bounds(door)
    # the door is 1.69 m against a 1.99 m body (the freezer lip shows above it): stretch it to the full front
    k = (bhi.z - blo.z - 0.01) / (hi.z - lo.z)
    door.matrix_world = (Matrix.Translation(Vector((0, 0, lo.z))) @ Matrix.Scale(k, 4, Vector((0, 0, 1)))
                         @ Matrix.Translation(Vector((0, 0, -lo.z))) @ door.matrix_world)
    bpy.context.view_layer.update()
    lo, hi = bounds(door)
    move(door, Vector(((blo.x + bhi.x) / 2 - (lo.x + hi.x) / 2, blo.y - hi.y + 0.005, blo.z + 0.005 - lo.z)))


def washer(objs):
    body, lid = objs['Washingmachine'], objs['Washingmachine_lid']
    blo, bhi = bounds(body)
    lo, hi = bounds(lid)
    rotate_about(lid, 'X', 90, (lo + hi) / 2)  # standing on edge at the back -> lying shut over the drum
    lo, hi = bounds(lid)
    depth = hi.y - lo.y
    y = blo.y + 0.04 + depth / 2
    z = top_of(body, blo.y + 0.04 + depth)
    move(lid, Vector(((blo.x + bhi.x) / 2 - (lo.x + hi.x) / 2, y - (lo.y + hi.y) / 2, z - lo.z)))


def toilet(objs):
    body = objs['Toilet']
    blo, bhi = bounds(body)
    rim = top_of(body, blo.y + 0.3)  # the bowl's rim: the highest point of the front 30 cm
    z = rim
    for name in ('Toilet_seat', 'Toilet_lid'):
        part = objs[name]
        lo, hi = bounds(part)
        rotate_about(part, 'X', 90, (lo + hi) / 2)  # standing up -> lying down
        lo, hi = bounds(part)
        move(part, Vector(((blo.x + bhi.x) / 2 - (lo.x + hi.x) / 2, blo.y + 0.015 - lo.y, z - lo.z)))
        z += hi.z - lo.z



def front_openings(body, step=0.01, recess=0.08):
    """The holes in a body's front (the -Y face here; the engine's +Z): rays fired straight in on a grid, cells that
    go deeper than `recess` past the front plane, grouped into rectangles. Returns ([(x0, x1, z0, z1)], frame_y):
    frame_y is the face the fronts sit flush with (the median of the shallow hits: the carcass, not a worktop lip)."""
    from mathutils.bvhtree import BVHTree
    lo, hi = bounds(body)
    deps = bpy.context.evaluated_depsgraph_get()
    tree = BVHTree.FromObject(body, deps)
    inv = body.matrix_world.inverted()
    nx, nz = int((hi.x - lo.x) / step), int((hi.z - lo.z) / step)
    deep = [[False] * nz for _ in range(nx)]
    shallow = []
    for i in range(nx):
        for j in range(nz):
            p = Vector((lo.x + (i + 0.5) * step, lo.y - 0.05, lo.z + (j + 0.5) * step))
            loc, _, _, _ = tree.ray_cast(inv @ p, (inv.to_3x3() @ Vector((0, 1, 0))).normalized())
            d = None if loc is None else (body.matrix_world @ loc).y - lo.y
            deep[i][j] = d is None or d > recess
            if d is not None and d <= recess:
                shallow.append(d)
    seen, out = set(), []
    for i in range(nx):
        for j in range(nz):
            if not deep[i][j] or (i, j) in seen:
                continue
            stack, cells = [(i, j)], []
            seen.add((i, j))
            while stack:
                a, b = stack.pop()
                cells.append((a, b))
                for c, d in ((a + 1, b), (a - 1, b), (a, b + 1), (a, b - 1)):
                    if 0 <= c < nx and 0 <= d < nz and deep[c][d] and (c, d) not in seen:
                        seen.add((c, d))
                        stack.append((c, d))
            i0, i1 = min(c[0] for c in cells), max(c[0] for c in cells) + 1
            j0, j1 = min(c[1] for c in cells), max(c[1] for c in cells) + 1
            # an opening is a filled rectangle that doesn't run off the body's edge (that would be the space
            # around legs, under a shelf...)
            if (len(cells) < 0.8 * (i1 - i0) * (j1 - j0) or (i1 - i0) * step < 0.1 or (j1 - j0) * step < 0.06
                    or i0 == 0 or j0 == 0 or i1 == nx or j1 == nz):
                continue
            out.append((lo.x + i0 * step, lo.x + i1 * step, lo.z + j0 * step, lo.z + j1 * step))
    shallow.sort()
    return out, lo.y + (shallow[len(shallow) // 2] if shallow else 0.0)


def fit_parts(body, parts, tol=0.15):
    """Seat each loose door / drawer in the body opening its front fits (flush with the body's front); a part can
    fill several openings (copies), and a pair of doors shares an opening twice their width. Unused parts are
    deleted; openings nothing fits stay open (shelves)."""
    blo, bhi = bounds(body)
    cx = (blo.x + bhi.x) / 2
    sizes = {}
    for p in parts:
        lo, hi = bounds(p)
        sizes[p.name] = (hi.x - lo.x, hi.z - lo.z)
    slots = []
    openings, frame_y = front_openings(body)
    for x0, x1, z0, z1 in openings:
        w, h = x1 - x0, z1 - z0
        single = [p for p in parts if abs(sizes[p.name][0] - w) < tol * w and abs(sizes[p.name][1] - h) < tol * h]
        pair = [p for p in parts if abs(2 * sizes[p.name][0] - w) < tol * w and abs(sizes[p.name][1] - h) < tol * h]
        if single:
            slots.append(((x0, x1, z0, z1), single))
        elif pair:
            m = (x0 + x1) / 2
            slots.append(((x0, m, z0, z1), pair))
            slots.append(((m, x1, z0, z1), pair))
    used = set()
    for (x0, x1, z0, z1), cands in slots:
        mid = (x0 + x1) / 2
        side = '_L' if mid < cx else '_R'
        # L / R doors are mirror images: the left one goes left of the body's centre
        cands = sorted(cands, key=lambda p: (not p.name.upper().endswith(side), abs(sizes[p.name][0] - (x1 - x0))))
        src = cands[0]
        part = src if src.name not in used else src.copy()
        if part is not src:
            bpy.context.scene.collection.objects.link(part)
        used.add(src.name)
        bpy.context.view_layer.update()
        lo, hi = bounds(part)
        move(part, Vector((mid - (lo.x + hi.x) / 2, frame_y - lo.y, (z0 + z1) / 2 - (lo.z + hi.z) / 2)))
    for p in parts:
        if p.name not in used:
            bpy.data.objects.remove(p, do_unlink=True)
    return len(slots)


# Body -> its loose part files (same folder). The B door sets are glass alternatives to the A ones: left out.
# Not listed: Kitchen_Cabinet_A, Nightstand_A, Dresser_Large_A, Bookshelf and Electricbox_Main, whose fronts the
# ray test can't read (no clean recessed openings) - use the B / C pieces, or place their parts by hand.
FITS = {
    'Furniture/Living_Room/Models/Tv_Stand.fbx': ['Tv_Stand_Door_L', 'Tv_Stand_Door_R'],
    'Furniture/Kitchen/Models/Kitchen_Counter_A.fbx': ['Kitchen_Counter_Door_L', 'Kitchen_Counter_Door_R',
                                                       'Kitchen_Counter_Drawer'],
    'Furniture/Kitchen/Models/Kitchen_Counter_Sink.fbx': ['Kitchen_Counter_Door_L', 'Kitchen_Counter_Door_R',
                                                          'Kitchen_Counter_Drawer'],
    'Furniture/Kitchen/Models/Kitchen_Cabinet_B.fbx': ['Kitchen_Cabinet_B_Door_A_L', 'Kitchen_Cabinet_B_Door_A_R'],
    'Furniture/Kitchen/Models/Kitchen_Cabinet_Large.fbx': ['Kitchen_Cabinet_Large_Door_A', 'Kitchen_Cabinet_Large_Door_B',
                                                           'Kitchen_Cabinet_Large_Door_C'],
    'Furniture/Bedroom/Models/Nightstand_B.fbx': ['Nightstand_B_Drawer_A', 'Nightstand_B_Drawer_B'],
    'Furniture/Bedroom/Models/Dresser_B.fbx': ['Dresser_B_Drawer_A'],
    'Furniture/Bedroom/Models/Dresser_C.fbx': ['Dresser_C_Drawer_A', 'Dresser_C_Drawer_B'],
    'Furniture/Bedroom/Models/Dresser_D.fbx': ['Dresser_D_Drawer_A', 'Dresser_D_Drawer_B', 'Dresser_D_Drawer_C'],
    'Furniture/Office/Models/Workdesk.fbx': ['Workdesk_Drawer_A'],
    'Furniture/Office/Models/Desk_Office.fbx': ['Desk_Office_Drawer'],
    'Furniture/Decor/Models/Cabinet_A.fbx': ['Cabinet_A_Door_A_L', 'Cabinet_A_Door_A_R'],
    'Furniture/Bathroom/Models/Bathroom_Sink_Cabinet.fbx': ['Bathroom_Sink_Door_L', 'Bathroom_Sink_Door_R'],
}


def shower(objs, door_left):
    """The stall ships as a bare frame; its sliding door and fixed glass panel lie off to the side. Close the
    front (the -Y side here) with them: the door at one end, the glass panel beside it."""
    stall = next(o for n, o in objs.items() if n.startswith('Shower_stall_190cm'))
    door, glass = objs['Shower_stall_door'], objs['Shower_stall_glass']
    slo, shi = bounds(stall)
    inset = 0.03
    for part, at_left in ((door, door_left), (glass, not door_left)):
        lo, hi = bounds(part)
        w = hi.x - lo.x
        x = slo.x + inset + w / 2 if at_left else shi.x - inset - w / 2
        move(part, Vector((x - (lo.x + hi.x) / 2, slo.y + 0.02 + (0.03 if part is door else 0.0) - lo.y,
                           slo.z + 0.1 - lo.z)))


def chandelier(objs):
    """The ring of candle shades is exported standing on edge beside the chandelier: lay it flat, centre it on the
    body's axis and sit the shades on the arms' cups (the top of the widest band of the body), wide end down."""
    body = next(o for n, o in objs.items() if n.endswith('_body'))
    shades = next(o for n, o in objs.items() if n.endswith('_shades'))
    lo, hi = bounds(shades)
    rotate_about(shades, 'X', 90, (lo + hi) / 2)
    blo, bhi = bounds(body)
    c = (blo + bhi) / 2
    verts = world_verts(body)
    # the arms: the horizontal slices where the body spreads widest from its axis
    n = 24
    step = (bhi.z - blo.z) / n
    spread = []
    for b in range(n):
        z0 = blo.z + b * step
        r = [((p.x - c.x) ** 2 + (p.y - c.y) ** 2) ** 0.5 for p in verts if z0 <= p.z < z0 + step]
        spread.append(max(r) if r else 0.0)
    widest = max(spread)
    arms_top = blo.z + (max(b for b in range(n) if spread[b] > 0.85 * widest) + 1) * step
    lo, hi = bounds(shades)
    move(shades, Vector((c.x - (lo.x + hi.x) / 2, c.y - (lo.y + hi.y) / 2, arms_top - 0.01 - lo.z)))


def bunk(objs):
    """Both blankets hang off the frame in the source and one pillow sits under the floor: drop the blankets and
    put that pillow on the lower bunk, matching the upper one."""
    from mathutils.bvhtree import BVHTree
    body = objs['Bed_bunk']
    for name in ('Blanket_A', 'Blanket_B'):
        bpy.data.objects.remove(objs[name], do_unlink=True)
    up, low = objs['Pillow_B'], objs['Pillow_A']
    ulo, uhi = bounds(up)
    tree = BVHTree.FromObject(body, bpy.context.evaluated_depsgraph_get())
    inv = body.matrix_world.inverted()
    c = (ulo + uhi) / 2
    # the lower mattress: straight down from just under the upper one
    hit, *_ = tree.ray_cast(inv @ Vector((c.x, c.y, ulo.z - 0.25)), (inv.to_3x3() @ Vector((0, 0, -1))).normalized())
    top = (body.matrix_world @ hit).z if hit else 0.35
    llo, lhi = bounds(low)
    move(low, Vector((c.x - (llo.x + lhi.x) / 2, c.y - (llo.y + lhi.y) / 2, top - llo.z)))


KITS = {
    'Furniture/Lighting/Models/Lamp_Table_A.fbx': lambda o: stand_shade(o['Lamp_table_A'], o['Lamp_table_A_shade']),
    'Furniture/Lighting/Models/Lamp_Table_B.fbx': lambda o: stand_shade(o['Lamp_table_B'], o['Lamp_table_B_shade']),
    'Furniture/Lighting/Models/Lamp_Floor_A.fbx': lambda o: stand_shade(o['Lamp_floor_A'], o['Lamp_floor_A_shade'], 0.6),
    'Furniture/Lighting/Models/Lamp_Table_Rocket.fbx':
        lambda o: stand_shade(o['Lamp_table_rocket_base'], o['Lamp_table_rocket_shade'], 0.5),
    'Furniture/Kitchen/Models/Fridge.fbx': fridge,
    'Furniture/Bathroom/Models/Washingmachine.fbx': washer,
    'Furniture/Bathroom/Models/Toilet.fbx': toilet,
    'Furniture/Bedroom/Models/Bed_Bunk.fbx': bunk,
    'Furniture/Lighting/Models/Lamp_Chandelier_A.fbx': chandelier,
    'Furniture/Lighting/Models/Lamp_Chandelier_B.fbx': chandelier,
    'Furniture/Bathroom/Models/Shower_Stall_190cm_L.fbx': (lambda o: shower(o, True), ['Shower_Stall_Door', 'Shower_Stall_Glass']),
    'Furniture/Bathroom/Models/Shower_Stall_190cm_R.fbx': (lambda o: shower(o, False), ['Shower_Stall_Door', 'Shower_Stall_Glass']),
}


def build():
    repo = sys.argv[sys.argv.index('--') + 1]
    only = sys.argv[sys.argv.index('--') + 2:]
    assets = os.path.join(repo, 'project', 'assets')
    jobs = [(rel, fix if callable(fix) else fix[0], [] if callable(fix) else fix[1]) for rel, fix in KITS.items()]
    jobs += [(rel, None, parts) for rel, parts in FITS.items()]
    for rel, fix, parts in jobs:
        if only and not any(o in rel for o in only):
            continue
        src = os.path.join(assets, *rel.split('/'))
        dst = src[:-4] + '_Assembled.fbx'
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.fbx(filepath=src)
        remap = {}
        body = [o for o in bpy.context.scene.objects if o.type == 'MESH' and o.parent is None][0]
        loose = []
        for stem in parts:
            before = set(bpy.context.scene.objects)
            part_src = os.path.join(os.path.dirname(src), stem + '.fbx')
            bpy.ops.import_scene.fbx(filepath=part_src)
            loose += [o for o in bpy.context.scene.objects if o not in before and o.type == 'MESH']
            with open(part_src + '.meta', encoding='utf-8') as f:
                remap.update(json.load(f).get('materialRemap', {}))
        # a second import renames a material it already has to "<name>.001": point the parts back at the original,
        # so the .meta's materialRemap still finds it by name
        for o in loose:
            for slot in o.material_slots:
                base = slot.material.name.rsplit('.', 1)[0] if slot.material else None
                if base and base != slot.material.name and base in bpy.data.materials:
                    slot.material = bpy.data.materials[base]
        if fix:
            fix({o.name: o for o in bpy.context.scene.objects})
        else:
            n = fit_parts(body, loose)
            print('assemble_kits: fitted', os.path.basename(rel), n, 'openings')
        bpy.ops.export_scene.fbx(filepath=dst, use_selection=False, object_types={'MESH', 'EMPTY'},
                                 apply_scale_options='FBX_SCALE_UNITS', bake_space_transform=False,
                                 path_mode='STRIP', embed_textures=False, add_leaf_bones=False, bake_anim=False)
        with open(src + '.meta', encoding='utf-8') as f:
            meta = json.load(f)
        old = None
        if os.path.exists(dst + '.meta'):
            with open(dst + '.meta', encoding='utf-8') as f:
                old = json.load(f).get('guid')
        meta['guid'] = old or secrets.token_hex(8)
        meta['materialRemap'] = {**remap, **meta.get('materialRemap', {})}
        with open(dst + '.meta', 'w', encoding='utf-8', newline='\n') as f:
            json.dump(meta, f, indent=2, sort_keys=True)
            f.write('\n')
        print('assemble_kits:', os.path.relpath(dst, repo))


def main():
    import argparse
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from import_model_library import model_report, write_json
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('only', nargs='*')
    ap.add_argument('--blender', default=(glob.glob(r'C:\Program Files\Blender Foundation\*\blender.exe') or ['blender'])[-1])
    ap.add_argument('--exe')
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    out = subprocess.run([args.blender, '-b', '--factory-startup', '--python', os.path.abspath(__file__), '--', repo,
                          *args.only], capture_output=True, text=True, encoding='utf-8', errors='replace')
    made = [line.split(': ', 1)[1].strip() for line in out.stdout.splitlines() if line.startswith('assemble_kits: ') and 'fitted' not in line]
    if out.returncode or not made:
        sys.exit('assemble_kits: Blender failed:\n' + out.stdout[-3000:] + out.stderr[-3000:])
    # Pivot: bottom centre of the assembled piece (the original's pivot was for its loose layout).
    exe = args.exe or os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe')
    os.chdir(repo)
    report = model_report(exe, made)
    for rel in made:
        r = report[os.path.normcase(os.path.normpath(rel))]
        with open(rel + '.meta', encoding='utf-8') as f:
            meta = json.load(f)
        old = meta['importer'].get('pivotOffset', [0.0, 0.0, 0.0])
        lo, hi = r['min'], r['max']
        # hanging fixtures hang from their top; everything else stands on its bottom centre
        meta['importer']['pivotOffset'] = [round(old[0] - (lo[0] + hi[0]) / 2, 4),
                                           round(old[1] - (hi[1] if 'Chandelier' in rel else lo[1]), 4),
                                           round(old[2] - (lo[2] + hi[2]) / 2, 4)]
        write_json(rel + '.meta', meta)
        print('assemble_kits:', rel)


if bpy:
    build()
elif __name__ == '__main__':
    main()
