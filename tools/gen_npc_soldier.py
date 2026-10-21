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
sandbox = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scenes', 'sandbox_template.json'), encoding='utf-8'))
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
# Wounded (NpcDirector::BecomeWounded, NpcBody::TickWounded). The clips are the MC Sample pack's (Raw\Animations\Mc_Sample),
# played as authored, in place (the capsule leads), each loop at the rate its own travel matches the capsule's (PlayRate) and
# the starts and stops distance-matched, as the standing locomotion. Cut by their .meta Clip Trims; one trim per file, so a take
# used for several states is in the project once per state, under the state's name.
#
# Shot down by a leg: GoProne collapses him clutching his belly onto the floor (ProneDrop: the collapse without its first
# standing second or the roll onto his side at the end; a leg shot falls as a ragdoll instead and blends straight into ProneIdle),
# then he lies still (ProneIdle: the prone pose the stand-to-prone clip settles in, played once and held - its quarter-second
# trim doesn't loop clean, the hands jumped at every wrap - the same as the crawl's first frame) or crawls on his belly:
# ProneCrawlStart (the take's frames 8-60)
# into ProneCrawl (one arm cycle, frames 60-113, 0.68 m/s) and ProneCrawlStop (113-182) back to still. He never gets up.
#
# Gut-shot: GoDying - the gun gone - doubled over clutching the wound (InjuredIdle), a few steps forward the same way
# (InjuredWalkStart, InjuredWalk at 0.68 m/s, InjuredWalkStop: their ends are the idle's and the walk's own poses), then
# Collapse: down to his knees and over onto his side (InjuredDrop, the whole collapse) to lie there (InjuredLying).
param('GoProne', 'trigger')
param('GoDying', 'trigger')
param('Collapse', 'trigger')
clips = {'ProneDrop': 'Injured/AM_InjuredBelly_DropToFloor', 'ProneIdle': 'ProneCrawl/AM_Stand_Trans_ProneCrawl',
         'ProneCrawlStart': 'ProneCrawl/AM_ProneCrawl_Start', 'ProneCrawl': 'ProneCrawl/AM_ProneCrawl_CrawlStartStop_Fwd',
         'ProneCrawlStop': 'ProneCrawl/AM_ProneCrawl_Stop',
         'InjuredIdle': 'Injured/AM_InjuredBelly_Idle_01', 'InjuredWalkStart': 'Injured/AM_InjuredBelly_Loco_Walk_Fwd_Start',
         'InjuredWalk': 'Injured/AM_InjuredBelly_Loco_Walk_Fwd', 'InjuredWalkStop': 'Injured/AM_InjuredBelly_Loco_Walk_Fwd_Stop',
         'InjuredDrop': 'Injured/AM_InjuredBelly_DropToFloor_Lie', 'InjuredLying': 'Injured/AM_InjuredBelly_DropToFloor_Loop'}
for name in clips.values():
    if not os.path.exists(os.path.join(proj, MOCAP, name + '.fbx')):
        fail(f'missing clip {name}.fbx')
base = ctrl['layers'][0]
base['states'] = [s for s in base['states'] if s['name'] not in clips]
base['transitions'] = [t for t in base['transitions'] if t.get('from') not in clips and t.get('to') not in clips]

def state(name, x, y, loop, tags, **extra):
    st = {'name': name, 'loop': loop, 'speed': 1.0, 'position': [x, y], 'tags': tags,
          'motions': {'main': {'clip': MOCAP + clips[name] + '.fbx'}}}
    st.update(extra)
    return st

def go(frm, to, duration, conditions=(), exit_time=None, **extra):
    t = {'from': frm, 'to': to, 'duration': duration,
         'conditions': [{'mode': m, 'param': p, 'threshold': 0.0} for m, p in conditions],
         'hasExitTime': exit_time is not None, 'exitTime': 1.0 if exit_time is None else exit_time}
    t.update(extra)
    return t

start = {'distanceParam': 'StartDistance', 'distanceMode': 'traveled'}
stop = {'distanceParam': 'StopDistance', 'distanceMode': 'remaining'}
base['states'] += [
    state('ProneDrop', 0.0, 600.0, False, ['Prone']),
    state('ProneIdle', 260.0, 600.0, False, ['Prone']),
    state('ProneCrawlStart', 520.0, 520.0, False, ['Prone'], **start),
    state('ProneCrawl', 780.0, 600.0, True, ['Prone'], speedParam='PlayRate'),
    state('ProneCrawlStop', 520.0, 680.0, False, ['Prone'], **stop),
    state('InjuredIdle', 260.0, 840.0, True, ['Dying']),
    state('InjuredWalkStart', 520.0, 760.0, False, ['Dying'], **start),
    state('InjuredWalk', 780.0, 840.0, True, ['Dying'], speedParam='PlayRate'),
    state('InjuredWalkStop', 520.0, 920.0, False, ['Dying'], **stop),
    state('InjuredDrop', 1040.0, 840.0, False, ['Dying']),
    state('InjuredLying', 1300.0, 840.0, True, ['Dying']),
]
base['transitions'] += [
    go('Any', 'ProneDrop', 0.3, [('if', 'GoProne')], canTransitionToSelf=False, interruptible=False),
    go('ProneDrop', 'ProneIdle', 0.5, exit_time=0.92),
    go('ProneIdle', 'ProneCrawlStart', 0.3, [('if', 'Moving')]),
    go('ProneCrawlStart', 'ProneCrawlStop', 0.3, [('ifNot', 'Moving')]),
    go('ProneCrawlStart', 'ProneCrawl', 0.2, exit_time=0.97),
    go('ProneCrawl', 'ProneCrawlStop', 0.3, [('ifNot', 'Moving')]),
    go('ProneCrawlStop', 'ProneCrawlStart', 0.3, [('if', 'Moving')]),
    go('ProneCrawlStop', 'ProneIdle', 0.3, exit_time=0.95),
    go('Any', 'InjuredIdle', 0.35, [('if', 'GoDying')], canTransitionToSelf=False, interruptible=False),
    go('Any', 'InjuredDrop', 0.3, [('if', 'Collapse')], canTransitionToSelf=False, interruptible=False),
    go('InjuredIdle', 'InjuredWalkStart', 0.25, [('if', 'Moving')]),
    go('InjuredWalkStart', 'InjuredWalkStop', 0.25, [('ifNot', 'Moving')]),
    go('InjuredWalkStart', 'InjuredWalk', 0.15, exit_time=0.97),
    go('InjuredWalk', 'InjuredWalkStop', 0.25, [('ifNot', 'Moving')]),
    go('InjuredWalkStop', 'InjuredWalkStart', 0.25, [('if', 'Moving')]),
    go('InjuredWalkStop', 'InjuredIdle', 0.25, exit_time=0.95),
    go('InjuredDrop', 'InjuredLying', 0.4, exit_time=0.96),
]
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
# A start (or a turn on the spot) given up mid-step - he no longer wants to move - eases out over 0.3 s, not the player's 0.15:
# a soldier's goal flips far more often than a player's stick, and caught with a foot up, the quick fade swung it 20 cm a frame.
for t in ctrl['layers'][0]['transitions']:
    frm = t.get('from') or ''
    if (frm.startswith('Start') or frm.startswith('CrouchStart')) and t.get('to') in ('Locomotion', 'CrouchLoco') and \
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
