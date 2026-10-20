# Which library pieces open, and how: the engine's Openable component (src/Game/Openable.h), for the scene generators.
# A kit's assembled body lists its parts in kit_parts.json (written by assemble_kits.py), each a file that lines up at the
# body's origin; a loose part (seated on its body by a generator) opens the way its name says.
import json
import os

MANIFEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kit_parts.json')
_manifest = None

# Loose single doors whose name doesn't give the hinge side.
LOOSE_HINGE = {'Electricbox_Door': 'Hinge Left', 'Kitchen_Cabinet_Corner_Door_A': 'Hinge Left',
               'Kitchen_Cabinet_Corner_Door_B': 'Hinge Left'}


def kit_parts(stem):
    """[{'stem', 'motion'}] of an assembled body (assemble_kits.py), each placed as a child at its origin; [] for
    anything else."""
    global _manifest
    if _manifest is None:
        with open(MANIFEST, encoding='utf-8') as f:
            _manifest = json.load(f)
    return _manifest.get(stem, [])


def loose_motion(stem):
    """How a loose part file opens, from its name ('Nightstand_A_Drawer_B' slides out), or None (it doesn't)."""
    words = stem.split('_')
    if stem in LOOSE_HINGE:
        return LOOSE_HINGE[stem]
    if 'Drawer' in words:
        return 'Slide Out'
    if 'Lid' in words:
        return 'Lid'
    if 'Door' in words:
        return 'Hinge Right' if words[-1] == 'R' else 'Hinge Left'
    return None


def component(motion):
    """The scene file's Openable block."""
    return {'Motion': motion}
