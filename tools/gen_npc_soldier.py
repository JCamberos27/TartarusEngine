# Generates the enemy soldier's body and its Animator Controller from the player's:
#   project/assets/AI/Soldier.json             - an entity fragment (the scene format): a root with the
#                                                Character Outfit and the Quantum outfit pieces, copied from
#                                                the Sandbox's Player Body without its First Person Body
#   project/assets/AI/npc_soldier.controller   - fps_body_locomotion.controller plus what an NPC needs on top
# NpcSpawner stamps the fragment out once per soldier. Re-run after changing the player's outfit or the
# body's controller. Usage: python tools/gen_npc_soldier.py <repo root>
import copy
import json
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
proj = os.path.join(root, 'project')
sandbox = json.load(open(os.path.join(proj, 'scenes', 'Sandbox.json'), encoding='utf-8'))
out_dir = os.path.join(proj, 'assets', 'AI')
os.makedirs(out_dir, exist_ok=True)

BODY_CONTROLLER = 'assets/Animations/Controllers/fps_body_locomotion.controller'
NPC_CONTROLLER = 'assets/AI/npc_soldier.controller'

def fail(msg):
    sys.exit(f'gen_npc_soldier: {msg}')

# --- the controller ---------------------------------------------------------------------------------
ctrl = json.load(open(os.path.join(proj, BODY_CONTROLLER), encoding='utf-8'))
params = {p['name'] for p in ctrl['parameters']}

def param(name, kind, default=0.0):
    if name not in params:
        ctrl['parameters'].append({'name': name, 'type': kind, 'default': default})
        params.add(name)

MOCAP = 'assets/Animations/Mocap/RootMotion/'
# Flinch: a light bump from the side the round came from, added over whatever the body is doing (HitX
# right, HitY forward: where the hit pushes the body). Upper body only, so the feet keep their step.
param('Hit', 'trigger')
param('HitX', 'float', 0.0)
param('HitY', 'float', -1.0)
bumps = [('AM_Stand_React_Bump_Light_Front', 0.0, -1.0), ('AM_Stand_React_Bump_Light_Back', 0.0, 1.0),
         ('AM_Stand_React_Bump_Light_Front_Left', 0.7, -0.7), ('AM_Stand_React_Bump_Light_Front_Right', -0.7, -0.7),
         ('AM_Stand_React_Bump_Light_Back_Left', 0.7, 0.7), ('AM_Stand_React_Bump_Light_Back_Right', -0.7, 0.7)]
for name, _, _ in bumps:
    if not os.path.exists(os.path.join(proj, MOCAP, 'React_Bump', name + '.fbx')):
        fail(f'missing clip React_Bump/{name}.fbx')
ctrl['layers'] = [l for l in ctrl['layers'] if l.get('name') != 'Hit React']
ctrl['layers'].append({
    'name': 'Hit React',
    'weight': 0.8,
    'blending': 'additive',
    'maskInclude': ['spine_01'],
    'defaultState': 'None',
    'states': [
        {'name': 'None', 'loop': True, 'speed': 1.0, 'position': [0.0, 0.0], 'tags': []},
        {'name': 'Flinch', 'loop': False, 'speed': 1.3, 'position': [0.0, 120.0], 'tags': ['Hit'],
         'motions': {'main': {'blendParam': 'HitX', 'blendParamY': 'HitY', 'children': [
             {'clip': MOCAP + 'React_Bump/' + n + '.fbx', 'threshold': x, 'thresholdY': y, 'speed': 1.0}
             for n, x, y in bumps]}}},
    ],
    'transitions': [
        {'from': 'Any', 'to': 'Flinch', 'duration': 0.06, 'conditions': [{'mode': 'if', 'param': 'Hit', 'threshold': 0.0}],
         'hasExitTime': False, 'exitTime': 1.0, 'canTransitionToSelf': True},
        {'from': 'Flinch', 'to': 'None', 'duration': 0.25, 'conditions': [], 'hasExitTime': True, 'exitTime': 0.8},
    ],
})
with open(os.path.join(proj, NPC_CONTROLLER), 'w', encoding='utf-8', newline='\n') as f:
    json.dump(ctrl, f, indent=2)
    f.write('\n')

# --- the body ---------------------------------------------------------------------------------------
src = {}
for kind in ('boxes', 'empties', 'models'):
    for e in sandbox[kind]:
        src[e['id']] = (kind, e)
body_ids = [i for i, (_, e) in src.items() if e['name'] == 'Player Body']
if len(body_ids) != 1: fail('expected one Player Body in Sandbox.json')
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
