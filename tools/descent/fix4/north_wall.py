"""north_wall re-apply script (Talos, fix-4).
Source: dev/fix3-north-wall d26b5d3c (commits 9ec9663d, acd6c995, d26b5d3c; history vs 9ec9663d^).
Stairwell hall line / north wall: open-air Bounds faces soft (living room, stair area, Lamp_table_A 16 top ramp, floor lamp 20). Main already carries part of this (9ec9663d/acd6c995 came in with #1213): those parts are skipped or advanced from the intermediate value.
Matches lights by NAME + POSITION + PARENT NAME (ids were renumbered by #1216). Each property lists the
prior values it may legitimately have (history of that key on the source branch); a current value that is
the new one is skipped, one of the prior ones is advanced, anything else aborts. Missing or ambiguous
entities abort too; nothing is written on failure. Adds no entities (the 36200 id block is unused).
Usage: python tools/descent/fix4/north_wall.py [path/to/Forest.json] [--dry]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix4_lib import apply_changes, scene_arg

CHANGES = [{'name': 'Ceiling Living room',
  'position': [-37.11668395996094, 3.3450000286102295, 8.541830062866211],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-33.464500427246094', '2.0380001068115234', '10.680000305175781']],
                              ['-33.464500427246094', '2.0380001068115234', '10.555000305175781']),
            'Bounds Fade -': ([None], ['-1.0', '-1.0', '0.5']),
            'Bounds Size': ([['10.390999794006348', '2.815000057220459', '5.800000190734863']],
                            ['10.390999794006348', '2.815000057220459', '6.050000190734863'])}},
 {'name': 'Stairwell Stairs',
  'position': [-36.58937072753906, 6.10099983215332, 3.7490499019622803],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-36.37099838256836', '3.492000102996826', '1.9399999380111694']],
                              ['-36.37099838256836', '3.492000102996826', '2.0649999380111694']),
            'Bounds Fade +': ([None], ['-1.0', '-1.0', '0.5']),
            'Bounds Size': ([['4.757998466491699', '5.724999904632568', '9.616000175476074']],
                            ['4.757998466491699', '5.724999904632568', '9.866000175476074'])}},
 {'name': 'Lamp_table_A Lamp_table_A 16',
  'position': [-36.90399932861328, 1.7314000129699707, 3.468899965286255],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-36.849998474121094', '2.08225', '1.975000023841858'],
                               ['-36.849998474121094', '2.08225', '2.100000023841858'],
                               ['-36.849998474121094', '2.20725', '2.100000023841858']],
                              ['-36.849998474121094', '2.10725', '2.100000023841858']),
            'Bounds Fade +': ([['0.6', '-1.0', '-1.0'], ['0.6', '-1.0', '0.5'], ['0.6', '0.5', '0.5']],
                              ['0.6', '0.23', '0.5']),
            'Bounds Size': ([['3.799999952316284', '2.9055', '5.849999904632568'],
                             ['3.799999952316284', '2.9055', '6.099999904632568'],
                             ['3.799999952316284', '3.1555', '6.099999904632568']],
                            ['3.799999952316284', '2.9555', '6.099999904632568'])}},
 {'name': 'Lamp_floor Lamp_floor 20',
  'position': [-37.38959884643555, 4.466899871826172, 7.874499797821045],
  'parent': 'House Lights',
  'props': {'Bounds Center': ([['-36.32600021362305', '4.947000026702881', '6.541500091552734']],
                              ['-36.32600021362305', '4.947000026702881', '6.416500091552734']),
            'Bounds Fade -': ([None], ['-1.0', '-1.0', '0.5']),
            'Bounds Size': ([['4.667998790740967', '2.815000057220459', '3.322999954223633']],
                            ['4.667998790740967', '2.815000057220459', '3.572999954223633'])}}]

# Stairwell Stairs is touched again by fam3_35604.py (applied after this one): treat its final values as "already".
for _c in CHANGES:
    if _c["name"] == "Stairwell Stairs":
        _p = _c["props"]
        _p["Bounds Center"] = _p["Bounds Center"] + ([["-36.24599838256836", "3.492000102996826", "2.0649999380111694"]],)
        _p["Bounds Fade +"] = _p["Bounds Fade +"] + ([["0.5", "-1.0", "0.5"]],)
        _p["Bounds Size"] = _p["Bounds Size"] + ([["5.007998466491699", "5.724999904632568", "9.866000175476074"]],)

if __name__ == "__main__":
    apply_changes(CHANGES, scene_arg(), label="north_wall", dry="--dry" in sys.argv)
