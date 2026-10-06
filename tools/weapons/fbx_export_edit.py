"""Small FBX metadata/curve edits using Blender's standalone binary reader/writer.

Preserves meshes, skin weights, bone offsets and all unrelated authored curves.
Requires numpy and the FBX add-on shipped with Blender (no bpy or GUI needed).
"""
import importlib
import os
from pathlib import Path
import sys
import types

ADDON = Path(os.environ.get('TARTARUS_BLENDER_FBX_ADDON',
    'C:/Program Files/Blender Foundation/Blender 5.2/5.2/scripts/addons_core/io_scene_fbx'))
package = types.ModuleType('tartarus_fbx')
package.__path__ = [str(ADDON)]
sys.modules[package.__name__] = package
parser = importlib.import_module('tartarus_fbx.parse_fbx')
encoder = importlib.import_module('tartarus_fbx.encode_bin')


def load(path):
    return parser.parse(str(path))


def child(node, name):
    return next(n for n in node.elems if n.id == name)


def properties(node):
    return {p.props[0]: p for p in child(node, b'Properties70').elems if p.id == b'P'}


def objects(root):
    return {n.props[0]: n for n in child(root, b'Objects').elems}


def models(root):
    return {n.props[1].split(b'\x00')[0]: n for n in objects(root).values() if n.id == b'Model'}


def curves(root, name, property_name):
    obs = objects(root)
    model_id = models(root)[name].props[0]
    connections = [n.props for n in child(root, b'Connections').elems]
    nodes = {c[1] for c in connections if c[2] == model_id and len(c) > 3 and c[3] == property_name}
    return {c[3]: obs[c[1]] for c in connections if c[2] in nodes and len(c) > 3 and obs[c[1]].id == b'AnimationCurve'}


def set_duration(root, seconds):
    stop = round(seconds * 46186158000)
    for node in objects(root).values():
        if node.id == b'AnimationStack':
            for key in (b'LocalStop', b'ReferenceStop'):
                properties(node)[key].props[4] = stop
    for node in root.elems:
        if node.id == b'Takes':
            for take in node.elems:
                if take.id == b'Take':
                    for span in take.elems:
                        if span.id in (b'LocalTime', b'ReferenceTime'):
                            span.props[:] = [0, stop]


def zero_translation(root, name):
    properties(models(root)[name])[b'Lcl Translation'].props[4:] = [0.0, 0.0, 0.0]
    for curve in curves(root, name, b'Lcl Translation').values():
        values = child(curve, b'KeyValueFloat').props[0]
        # The root is neutral in these exports; preserve all moving child bones.
        if max(values, default=0) - min(values, default=0) > 1e-4:
            raise ValueError(f'{name!r} has animated root translation')
        for i in range(len(values)):
            values[i] = 0.0


def write(path, root, version):
    methods = {b'C': 'add_char', b'B': 'add_bool', b'Z': 'add_int8', b'Y': 'add_int16', b'I': 'add_int32', b'L': 'add_int64',
               b'F': 'add_float32', b'D': 'add_float64', b'R': 'add_bytes', b'S': 'add_string',
               b'f': 'add_float32_array', b'd': 'add_float64_array', b'i': 'add_int32_array',
               b'l': 'add_int64_array', b'b': 'add_bool_array', b'c': 'add_byte_array'}
    def convert(src):
        dest = encoder.FBXElem(src.id)
        for kind, value in zip(src.props_type, src.props):
            getattr(dest, methods[bytes([kind])])(value)
        dest.elems = [convert(n) for n in src.elems]
        return dest
    encoder.write(str(path), convert(root), version)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        root, version = load(path)
        print(path, version)
        for n in objects(root).values():
            if n.id == b'AnimationStack':
                print('stack', {k.decode(): p.props[4:] for k,p in properties(n).items()})
        for name in (b'root', b'Main', b'mag2'):
            if name in models(root):
                print(name, {k.decode(): p.props[4:] for k,p in properties(models(root)[name]).items() if k.startswith(b'Lcl')})
                for prop in (b'Lcl Translation', b'Lcl Rotation'):
                    print(prop, {k.decode(): (list(child(v,b'KeyTime').props[0])[:2], list(child(v,b'KeyValueFloat').props[0])[:2]) for k,v in curves(root,name,prop).items()})
