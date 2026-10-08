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



KITS = {
    'Furniture/Lighting/Models/Lamp_Table_A.fbx': lambda o: stand_shade(o['Lamp_table_A'], o['Lamp_table_A_shade']),
    'Furniture/Lighting/Models/Lamp_Table_B.fbx': lambda o: stand_shade(o['Lamp_table_B'], o['Lamp_table_B_shade']),
    'Furniture/Lighting/Models/Lamp_Floor_A.fbx': lambda o: stand_shade(o['Lamp_floor_A'], o['Lamp_floor_A_shade'], 0.6),
    'Furniture/Lighting/Models/Lamp_Table_Rocket.fbx':
        lambda o: stand_shade(o['Lamp_table_rocket_base'], o['Lamp_table_rocket_shade'], 0.5),
    'Furniture/Kitchen/Models/Fridge.fbx': fridge,
    'Furniture/Bathroom/Models/Washingmachine.fbx': washer,
    'Furniture/Bathroom/Models/Toilet.fbx': toilet,
}


def build():
    repo = sys.argv[sys.argv.index('--') + 1]
    only = sys.argv[sys.argv.index('--') + 2:]
    assets = os.path.join(repo, 'project', 'assets')
    for rel, fix in KITS.items():
        if only and not any(o in rel for o in only):
            continue
        src = os.path.join(assets, *rel.split('/'))
        dst = src[:-4] + '_Assembled.fbx'
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.fbx(filepath=src)
        objs = {o.name: o for o in bpy.context.scene.objects}
        fix(objs)
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
    made = [line.split(': ', 1)[1].strip() for line in out.stdout.splitlines() if line.startswith('assemble_kits: ')]
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
        meta['importer']['pivotOffset'] = [round(old[0] - (lo[0] + hi[0]) / 2, 4), round(old[1] - lo[1], 4),
                                           round(old[2] - (lo[2] + hi[2]) / 2, 4)]
        write_json(rel + '.meta', meta)
        print('assemble_kits:', rel)


if bpy:
    build()
elif __name__ == '__main__':
    main()
