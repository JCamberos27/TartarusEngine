"""fam4_south re-apply script (Talos, fix-4).
Source: dev/fix3-fam4-south 525e3a75 (commits 422f7a81, 525e3a75), incl. the Ceiling Kitchen (old 35590) change.
Family 4 south: inward fade on Ceiling Kitchen (-z face 20.3 -> 20.05 + fade 0.5) and Lamp_stand 19 (-z fade 0.4).
Matches lights by NAME + POSITION + PARENT NAME (ids were renumbered by #1216). Each property lists the
prior values it may legitimately have (history of that key on the source branch); a current value that is
the new one is skipped, one of the prior ones is advanced, anything else aborts. Missing or ambiguous
entities abort too; nothing is written on failure. Adds no entities (the 36200 id block is unused).
Usage: python tools/descent/fix4/fam4_south.py [path/to/Forest.json] [--dry]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix4_lib import apply_changes, scene_arg

CHANGES = [{'name': 'Ceiling Kitchen',
  'position': [-29.561721801757812, 3.3450000286102295, -13.807950019836426],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-32.97999954223633', '2.0380001068115234', '-11.579999923706055']],
                              ['-32.85499954223633', '2.0380001068115234', '-11.579999923706055']),
            'Bounds Fade +': ([None], ['0.5', '-1.0', '-1.0']),
            'Bounds Size': ([['11.359999656677246', '2.815000057220459', '7.599999904632568']],
                            ['11.609999656677246', '2.815000057220459', '7.599999904632568'])}},
 {'name': 'Lamp_stand Lamp_stand 19',
  'position': [-28.978300094604492, 1.8681000471115112, -13.76609992980957],
  'parent': 'House Lights',
  'props': {'Bounds Fade +': ([None], ['0.4', '-1.0', '-1.0'])}}]

if __name__ == "__main__":
    apply_changes(CHANGES, scene_arg(), label="fam4_south", dry="--dry" in sys.argv)
