# Generates the training dummy's body and its Animator Controller from the player's:
#   project/assets/AI/Soldier.json             - an entity fragment (the scene format): a root with the
#                                                Character Outfit and the Quantum outfit pieces, copied from
#                                                the Sandbox's Player Body without its First Person Body
#   project/assets/AI/npc_soldier.controller   - fps_body_locomotion.controller less what a dummy never plays (crouch,
#                                                jump, armed fidgets), its given-up starts eased out slower
# NpcSpawner stamps the fragment out once per dummy. Re-run after changing the player's outfit or the
# body's controller. Usage: python tools/gen_npc_soldier.py <repo root>
import copy
import json
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
proj = os.path.join(root, 'project')
sandbox = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scenes', 'sandbox_template.json'), encoding='utf-8'))
out_dir = os.path.join(proj, 'assets', 'AI')
os.makedirs(out_dir, exist_ok=True)

BODY_CONTROLLER = 'assets/Animations/Controllers/fps_body_locomotion.controller'
NPC_CONTROLLER = 'assets/AI/npc_soldier.controller'

def fail(msg):
    sys.exit(f'gen_npc_soldier: {msg}')

# --- the controller ---------------------------------------------------------------------------------
ctrl = json.load(open(os.path.join(proj, BODY_CONTROLLER), encoding='utf-8'))
# What a dummy never does is left out: it never crouches (no crouch half), never jumps (Jump; falling off a ledge still
# plays Fall and Land) and is unarmed (the fidget blend's armed idles, FidgetIndex 0 and 1, are never picked).
# NpcBody::Tick keeps Crouched and Jumped at 0. The flinch is procedural (NpcFlinch on the pose), not a layer here.
base = ctrl['layers'][0]
gone = {st['name'] for st in base['states'] if 'Crouch' in st.get('tags', []) or st['name'] == 'Jump'}
base['states'] = [st for st in base['states'] if st['name'] not in gone]
base['transitions'] = [t for t in base['transitions'] if t.get('from') not in gone and t.get('to') not in gone]
for st in base['states']:
    if st['name'] == 'Fidget':
        main = st['motions']['main']
        main['children'] = [c for c in main['children'] if c['threshold'] >= 2.0]
if base.get('defaultState') in gone: fail('the base layer starts in a state a dummy never uses')
unused = {'Crouched', 'CrouchDown', 'CrouchUp', 'Jump'}
ctrl['parameters'] = [p for p in ctrl['parameters'] if p['name'] not in unused]
# A start (or a turn on the spot) given up mid-step - he no longer wants to move - eases out over 0.3 s, not the player's 0.15:
# a soldier's goal flips far more often than a player's stick, and caught with a foot up, the quick fade swung it 20 cm a frame.
for t in ctrl['layers'][0]['transitions']:
    frm = t.get('from') or ''
    if frm.startswith('Start') and t.get('to') == 'Locomotion' and \
            [(c['param'], c['mode']) for c in t.get('conditions', [])] == [('Moving', 'ifNot')]:
        t['duration'] = max(t.get('duration', 0.0), 0.3)
with open(os.path.join(proj, NPC_CONTROLLER), 'w', encoding='utf-8', newline='\n') as f:
    json.dump(ctrl, f, indent=2)
    f.write('\n')

# --- the body ---------------------------------------------------------------------------------------
src = {}
for kind in ('boxes', 'empties', 'models'):
    for e in sandbox[kind]:
        src[e['id']] = (kind, e)
body_ids = [i for i, (_, e) in src.items() if e['name'] == 'Player Body']
if len(body_ids) != 1: fail('expected one Player Body in sandbox_template.json')
body_id = body_ids[0]
_, body = src[body_id]
if 'Character Outfit' not in body: fail('Player Body has no Character Outfit')
pieces = sorted((e for _, (_, e) in src.items() if e.get('parentId') == body_id), key=lambda e: e['order'])
if not pieces: fail('Player Body has no pieces')

frag = {'formatVersion': 4, 'boxes': [], 'empties': [], 'models': []}
rootent = {'id': 0, 'name': 'Soldier', 'order': 0, 'parentId': -1, 'position': [0, 0, 0], 'rotation': [0, 0, 0, 1],
           'scale': [1, 1, 1], 'Character Outfit': copy.deepcopy(body['Character Outfit'])}
rootent['Character Outfit']['Randomize On Play'] = False
frag['empties'].append(rootent)
for k, p in enumerate(pieces):
    c = copy.deepcopy(p)
    c['id'] = k + 1
    c['order'] = k + 1
    c['parentId'] = 0
    ac = c.get('Animator Controller')
    if not ac: fail(f'piece {p["name"]} has no Animator Controller')
    ac['Controller'] = NPC_CONTROLLER
    ac['rootMotion'] = 'In Place'
    frag['models'].append(c)
with open(os.path.join(out_dir, 'Soldier.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump(frag, f, indent=2, sort_keys=True)
    f.write('\n')
print(f'wrote {NPC_CONTROLLER} ({len(ctrl["layers"])} layers) and assets/AI/Soldier.json ({len(pieces)} pieces)')
