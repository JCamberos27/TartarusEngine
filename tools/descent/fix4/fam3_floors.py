"""fam3_floors re-apply script (Talos, fix-4).
Source: dev/fix3-fam3-floors 84fd8c5d (commits a7756697, 84fd8c5d).
Family 3: Lamp_stand 10 (old 35605) Bounds +z 26.92 -> 29.472 (child bedroom closet wall).
Matches lights by NAME + POSITION + PARENT NAME (ids were renumbered by #1216). Each property lists the
prior values it may legitimately have (history of that key on the source branch); a current value that is
the new one is skipped, one of the prior ones is advanced, anything else aborts. Missing or ambiguous
entities abort too; nothing is written on failure. Adds no entities (the 36200 id block is unused).
Usage: python tools/descent/fix4/fam3_floors.py [path/to/Forest.json] [--dry]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix4_lib import apply_changes, scene_arg

CHANGES = [{'name': 'Lamp_stand Lamp_stand 10',
  'position': [-31.44890022277832, 4.763999938964844, 10.453800201416016],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-32.54999923706055', '4.947000026702881', '10.414999961853027'],
                               ['-33.75', '4.947000026702881', '10.414999961853027']],
                              ['-33.826', '4.947000026702881', '10.414999961853027']),
            'Bounds Size': ([['2.740000009536743', '2.815000057220459', '6.190000057220459'],
                             ['5.139999866485596', '2.815000057220459', '6.190000057220459']],
                            ['5.292', '2.815000057220459', '6.190000057220459'])}}]

if __name__ == "__main__":
    apply_changes(CHANGES, scene_arg(), label="fam3_floors", dry="--dry" in sys.argv)
