# Door_Op_1_Leaf carries ten brass number plates (Number_0..Number_9) stacked in one spot, meant to be shown one
# at a time. This exports each plate as its own model, Door_Number_<d>.fbx next to the leaf, so a scene can spell
# out an apartment number plate by plate. Each .meta is the leaf's (same scale, numbers material) with nothing
# excluded and a bottom-centre pivot; the plate's face is the model's +Z.
#
#   python tools/assets/door_numbers.py <repo> [--blender <blender.exe>] [--exe <TartarusEngine.exe>]
# runs itself inside Blender to cut the plates out, then re-centres each pivot from the engine's --model-report, so
# the exe must be built.
import glob
import json
import os
import secrets
import subprocess
import sys

try:
    import bpy
except ImportError:
    bpy = None

LEAF = 'project/assets/Architecture/Doors/Models/Door_Op_1_Leaf.fbx'


def build():
    repo = sys.argv[sys.argv.index('--') + 1]
    src = os.path.join(repo, *LEAF.split('/'))
    for d in range(10):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.fbx(filepath=src)
        keep = bpy.data.objects[f'Number_{d}']
        chain = {keep}
        p = keep.parent
        while p:
            chain.add(p)
            p = p.parent
        for o in list(bpy.data.objects):
            if o not in chain:
                bpy.data.objects.remove(o, do_unlink=True)
        dst = os.path.join(os.path.dirname(src), f'Door_Number_{d}.fbx')
        bpy.ops.export_scene.fbx(filepath=dst, use_selection=False, object_types={'MESH', 'EMPTY'},
                                 apply_scale_options='FBX_SCALE_UNITS', bake_space_transform=False,
                                 path_mode='STRIP', embed_textures=False, add_leaf_bones=False, bake_anim=False)
        print('door_numbers:', os.path.relpath(dst, repo).replace(os.sep, '/'))


def main():
    import argparse
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from import_model_library import model_report, write_json
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('--blender', default=(glob.glob(r'C:\Program Files\Blender Foundation\*\blender.exe') or ['blender'])[-1])
    ap.add_argument('--exe')
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    out = subprocess.run([args.blender, '-b', '--factory-startup', '--python', os.path.abspath(__file__), '--', repo],
                         capture_output=True, text=True, encoding='utf-8', errors='replace')
    made = [line.split(': ', 1)[1].strip() for line in out.stdout.splitlines() if line.startswith('door_numbers: ')]
    if out.returncode or len(made) != 10:
        sys.exit('door_numbers: Blender failed:\n' + out.stdout[-3000:] + out.stderr[-3000:])
    with open(os.path.join(repo, LEAF + '.meta'), encoding='utf-8') as f:
        leaf = json.load(f)
    os.chdir(repo)
    for rel in made:
        old = None
        if os.path.exists(rel + '.meta'):
            with open(rel + '.meta', encoding='utf-8') as f:
                old = json.load(f).get('guid')
        meta = json.loads(json.dumps(leaf))
        meta['guid'] = old or secrets.token_hex(8)
        meta['materialRemap'] = {'Numbers_Mat': leaf['materialRemap']['Numbers_Mat']}
        meta['importer']['excludeNodes'] = []
        meta['importer']['pivotOffset'] = [0.0, 0.0, 0.0]
        write_json(rel + '.meta', meta)
    # Pivot: bottom centre of the plate.
    exe = args.exe or os.path.join(repo, 'build', 'Release', 'TartarusEngine.exe')
    report = model_report(exe, made)
    for rel in made:
        lo, hi = report[os.path.normcase(os.path.normpath(rel))]['min'], report[os.path.normcase(os.path.normpath(rel))]['max']
        with open(rel + '.meta', encoding='utf-8') as f:
            meta = json.load(f)
        meta['importer']['pivotOffset'] = [round(-(lo[0] + hi[0]) / 2, 4), round(-lo[1], 4), round(-(lo[2] + hi[2]) / 2, 4)]
        write_json(rel + '.meta', meta)
        print('door_numbers:', rel, 'size', [round(b - a, 4) for a, b in zip(lo, hi)])


if bpy:
    build()
elif __name__ == '__main__':
    main()
