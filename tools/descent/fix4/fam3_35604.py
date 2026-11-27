"""fam3_35604 re-apply script (Talos, fix-4).
Source: dev/fix3-fam3-35604 5e3ba4c2.
Family 3 (b): Stairwell Stairs Bounds -z 26.992 -> 26.742 + per-face fade 0.5. Run after north_wall.py.
Matches lights by NAME + POSITION + PARENT NAME (ids were renumbered by #1216). Each property lists the
prior values it may legitimately have (history of that key on the source branch); a current value that is
the new one is skipped, one of the prior ones is advanced, anything else aborts. Missing or ambiguous
entities abort too; nothing is written on failure. Adds no entities (the 36200 id block is unused).
Usage: python tools/descent/fix4/fam3_35604.py [path/to/Forest.json] [--dry]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix4_lib import apply_changes, scene_arg

CHANGES = [{'name': 'Stairwell Stairs',
  'position': [-36.58937072753906, 6.10099983215332, 3.7490499019622803],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-36.37099838256836', '3.492000102996826', '2.0649999380111694']],
                              ['-36.24599838256836', '3.492000102996826', '2.0649999380111694']),
            'Bounds Fade +': ([['-1.0', '-1.0', '0.5']], ['0.5', '-1.0', '0.5']),
            'Bounds Size': ([['4.757998466491699', '5.724999904632568', '9.866000175476074']],
                            ['5.007998466491699', '5.724999904632568', '9.866000175476074'])}}]

if __name__ == "__main__":
    apply_changes(CHANGES, scene_arg(), label="fam3_35604", dry="--dry" in sys.argv)
