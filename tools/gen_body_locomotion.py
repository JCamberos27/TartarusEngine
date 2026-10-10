# Builds the first-person body's locomotion graph, the one animation set the player's body, its world twins
# and the NPC soldiers all play (MC Core Motion, root-motion clips):
#
#   project/assets/Animations/Controllers/fps_body_locomotion.template.json - the graph with clip roles
#       (file stems without "AM_"), what the editor's Create Body Locomotion Controller fills by name
#   project/assets/Animations/Controllers/fps_body_locomotion.controller    - the same graph, roles resolved
#       to the pack's files under assets/Animations/Mocap/RootMotion
#
# then run tools/gen_npc_soldier.py for the soldiers' copy. Usage: python tools/gen_body_locomotion.py <repo root>
#
# The game drives it from BodyLocomotion.cs (the player) and NpcBody (the soldiers). Travel is the capsule's
# (input-led): the starts, stops and pivots are distance matched (a state's distanceParam) so their feet keep
# pace with it, and the loops play at the rate the capsule moves.
import json
import os
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
PROJECT = os.path.join(ROOT, 'project')
CTRL_DIR = os.path.join(PROJECT, 'assets', 'Animations', 'Controllers')
CLIP_DIR = 'assets/Animations/Mocap/RootMotion'

params = []


def param(name, kind, default=0.0):
    params.append({'name': name, 'type': kind, 'default': float(default)})


for n, k, d in [
        ('MoveX', 'float', 0), ('MoveY', 'float', 0), ('Speed', 'float', 0), ('Sprint', 'bool', 0), ('Grounded', 'bool', 1),
        ('Airborne', 'bool', 0), ('Jump', 'trigger', 0), ('Turning', 'bool', 0), ('TurnAngle', 'float', 0),
        ('Moving', 'bool', 0), ('Start', 'trigger', 0), ('Stop', 'trigger', 0), ('StopRun', 'trigger', 0),
        ('StartX', 'float', 0), ('StartY', 'float', 1), ('StopX', 'float', 0), ('StopY', 'float', 1), ('Crouched', 'bool', 0),
        ('CrouchDown', 'trigger', 0), ('CrouchUp', 'trigger', 0), ('PlayRate', 'float', 1),
        # Gait of a start / stop / pivot: 0 walk, 1 jog, 2 run.
        ('StartGait', 'float', 1), ('StopGait', 'float', 1),
        # A start that turns the body: signed (+ left) and its size, in 45 degree steps (1 = 45, 4 = 180).
        ('StartTurn', 'float', 0), ('StartTurnAmount', 'float', 1),
        # Distance matching, metres: since the start, still to go in the stop, from the pivot's turnaround.
        ('StartDistance', 'float', 0), ('StopDistance', 'float', 0), ('PivotDistance', 'float', 0),
        ('Pivot', 'trigger', 0), ('PivotX', 'float', 0), ('PivotY', 'float', 1), ('PivotGait', 'float', 1),
        # A tap of the keys: a small step instead of a start and a stop.
        ('Step', 'trigger', 0), ('StepX', 'float', 0), ('StepY', 'float', 1),
        # Idle variety: which fidget (an index into the Fidget / CrouchFidget trees).
        ('Fidget', 'trigger', 0), ('FidgetIndex', 'float', 0),
        ('SprintRate', 'float', 1)]:
    param(n, k, d)

states = []
transitions = []


def clip(role):
    return {'clip': role}


def tree(px, py, children, speed=1.0):
    """children: (role, x, y) - a 2D tree, or (role, x) with py None - a 1D tree."""
    m = {'blendParam': px, 'children': []}
    if py:
        m['blendParamY'] = py
    for c in children:
        child = {'clip': c[0], 'threshold': float(c[1]), 'speed': float(c[3]) if len(c) > 3 else 1.0}
        if py:
            child['thresholdY'] = float(c[2])
        m['children'].append(child)
    return m


def state(name, motion, loop, tags, pos, speed=1.0, speed_param=None, distance=None):
    s = {'name': name, 'loop': loop, 'speed': speed, 'position': list(pos), 'tags': tags, 'motions': {'main': motion}}
    if speed_param:
        s['speedParam'] = speed_param
    if distance:
        s['distanceParam'], s['distanceMode'] = distance
    states.append(s)


def cond(param_name, mode, threshold=0.0):
    return {'mode': mode, 'param': param_name, 'threshold': float(threshold)}


def go(src, dst, duration, conds=(), exit_time=None, offset=0.0):
    t = {'from': src, 'to': dst, 'duration': duration, 'conditions': list(conds),
         'hasExitTime': exit_time is not None, 'exitTime': exit_time if exit_time is not None else 1.0}
    if offset:
        t['offset'] = offset
    transitions.append(t)


def cardinal(prefix, suffix):
    return [(f'{prefix}_Fwd{suffix}', 0, 1), (f'{prefix}_Bwd{suffix}', 0, -1), (f'{prefix}_Left{suffix}', -1, 0), (f'{prefix}_Right{suffix}', 1, 0)]


# --- states ------------------------------------------------------------------------------------------------
# The gait loops: x / y are the clips' measured travel (m/s) in the body's frame.
state('Locomotion', tree('MoveX', 'MoveY', [
    ('Ready_Idle_01', 0, 0),
    ('Loco_Walk_Fwd', 0, 1.53), ('Loco_Walk_Fwd_Left', -1.082, 1.082), ('Loco_Walk_Fwd_Right', 1.082, 1.082),
    ('Loco_Walk_Left', -1.346, 0), ('Loco_Walk_Right', 1.586, 0), ('Loco_Walk_Bwd', 0, -1.195),
    ('Loco_Walk_Bwd_Left', -0.845, -0.845), ('Loco_Walk_Bwd_Right', 0.845, -0.845),
    ('Loco_Jog_Fwd', 0, 3.264), ('Loco_Jog_Fwd_Left', -2.308, 2.308), ('Loco_Jog_Fwd_Right', 2.308, 2.308),
    ('Loco_Jog_Left', -2.3, 0), ('Loco_Jog_Right', 2.945, 0), ('Loco_Jog_Bwd', 0, -2.261),
    ('Loco_Jog_Bwd_Left', -1.599, -1.599), ('Loco_Jog_Bwd_Right', 1.599, -1.599),
    ('Loco_Run_Fwd', 0, 4.736)]), True, ['Locomotion'], (0, 0), speed_param='PlayRate')
# Sprint: its own loop (a faster cadence than the run), the legs squared to the travel (the body turns the
# torso back to the view); played at the capsule's speed over the clip's 4.25 m/s.
state('Sprint', clip('Loco_Run_Fast_01'), True, ['Locomotion', 'Sprint'], (0, -240), speed_param='SprintRate')
state('Jump', clip('Jump'), False, ['Airborne'], (260, -120))
state('Fall', clip('Jump_Fall_Loop'), True, ['Airborne'], (520, -120))
state('Land', clip('Jump_Land_Recovery'), False, [], (520, 0), speed=1.4)
# Turn on the spot; the body takes the clip's own yaw. The angle is never under 45 (a side's smallest clip), so
# the two sides' clips never mix.
TURN = [('Stand_Idle_Turn_R180', -180), ('Stand_Idle_Turn_R135', -135), ('Stand_Idle_Turn_R090', -90),
        ('Stand_Idle_Turn_R045', -45), ('Stand_Idle_Turn_L045', 45), ('Stand_Idle_Turn_L090', 90),
        ('Stand_Idle_Turn_L135', 135), ('Stand_Idle_Turn_L180', 180)]
state('Turn', tree('TurnAngle', None, TURN), False, ['Turn'], (0, 240))
# Starts, by gait and direction, distance matched to the capsule's travel since the push-off.
start = ('StartDistance', 'traveled')
state('StartWalk', tree('StartX', 'StartY', cardinal('Loco_Walk', '_Start')), False, ['Start'], (-260, 120), distance=start)
state('Start', tree('StartX', 'StartY', cardinal('Loco_Jog', '_Start')), False, ['Start'], (-260, 200), distance=start)
state('StartRun', clip('Loco_Run_Fwd_Start'), False, ['Start'], (-260, 280), distance=start)
# Starts that turn the body to its new heading on the way (x: 45 degree steps, y: gait). Each side's clips lead
# with that side's foot, so they blend; the sides never mix.
def start_turn(side):
    rows = []
    for gait, folder in ((0, 'Walk'), (1, 'Jog'), (2, 'Run')):
        for k, deg in enumerate(('045', '090', '135', '180')):
            rows.append((f'Loco_{folder}_Start_Turn_{side}{deg}_{side}foot', k + 1, gait))
    return rows


state('StartTurnL', tree('StartTurnAmount', 'StartGait', start_turn('L')), False, ['Start', 'StartTurn'], (-520, 120), distance=start)
state('StartTurnR', tree('StartTurnAmount', 'StartGait', start_turn('R')), False, ['Start', 'StartTurn'], (-520, 200), distance=start)
# Stops: matched to what the capsule has still to travel.
stop = ('StopDistance', 'remaining')
state('StopWalk', tree('StopX', 'StopY', cardinal('Loco_Walk', '_Stop')), False, ['Stop'], (260, 120), distance=stop)
state('Stop', tree('StopX', 'StopY', cardinal('Loco_Jog', '_Stop')), False, ['Stop'], (260, 200), distance=stop)
state('StopRun', clip('Loco_Run_Fwd_Stop'), False, ['Stop'], (260, 280), distance=stop)
# Pivots: the travel reverses without the body turning (x / y: the direction it was going).
pivot = ('PivotDistance', 'pivot')
state('PivotWalk', tree('PivotX', 'PivotY', cardinal('Loco_Walk', '_Pivot')), False, ['Pivot'], (260, -240), distance=pivot)
state('PivotJog', tree('PivotX', 'PivotY', cardinal('Loco_Jog', '_Pivot')), False, ['Pivot'], (520, -240), distance=pivot)
# A tap: one small step in its direction.
state('IdleStep', tree('StepX', 'StepY', [('Ready_Idle_Step_Fwd_L_Foot', 0, 1), ('Ready_Idle_Step_Bwd_R_Foot', 0, -1),
                                          ('Ready_Idle_Step_Left', -1, 0), ('Ready_Idle_Step_Right', 1, 0)]),
      False, ['Step'], (-260, -120), speed=1.35)
# Idle variety, one clip at a time (FidgetIndex an integer): 0-1 with a gun (the ready stance), 2-8 without.
FIDGETS = ['Ready_Idle_02', 'Ready_Idle_03_Look_Around', 'Stand_Idle_02', 'Stand_Idle_03_Look_Around',
           'Stand_Idle_04_Look_Around', 'Stand_Idle_06_Scratch_Arm', 'Stand_Idle_07_Scratch_Leg', 'Stand_Idle_08_Thigh_Tap',
           'Stand_Idle_11_Point']
state('Fidget', tree('FidgetIndex', None, [(c, i) for i, c in enumerate(FIDGETS)]), False, ['Fidget'], (-520, -120))
# Crouched.
state('CrouchLoco', tree('MoveX', 'MoveY', [
    ('Crouch_Idle_01', 0, 0),
    ('Crouch_Loco_Walk_Fwd', 0, 1.355), ('Crouch_Loco_Walk_Fwd_Left', -0.957, 0.957), ('Crouch_Loco_Walk_Fwd_Right', 0.957, 0.957),
    ('Crouch_Loco_Walk_Left', -1.155, 0), ('Crouch_Loco_Walk_Right', 1.133, 0), ('Crouch_Loco_Walk_Bwd', 0, -1.123),
    ('Crouch_Loco_Walk_Bwd_Left', -0.794, -0.794), ('Crouch_Loco_Walk_Bwd_Right', 0.794, -0.794)]),
    True, ['Crouch'], (0, 480), speed=1.5, speed_param='PlayRate')
CTURN = [(r.replace('Stand_Idle', 'Crouch_Idle'), a) for r, a in TURN]
state('CrouchTurn', tree('TurnAngle', None, CTURN), False, ['Crouch', 'Turn'], (0, 640))
state('CrouchDown', clip('Stand_Idle_Trans_Crouch_02'), False, ['Crouch'], (260, 400), speed=1.5)
state('CrouchUp', clip('Crouch_Idle_Trans_Stand_01'), False, ['Crouch'], (260, 480), speed=1.5)
state('CrouchStart', tree('StartX', 'StartY', cardinal('Crouch_Loco_Walk', '_Start')), False, ['Crouch', 'Start'], (-260, 400), distance=start)
# Crouched start-turns: the right side's 45 / 90 lead with the left foot and its 135 / 180 with the right, so
# the game asks for whole clips (StartTurnAmount 1..4) and nothing between them blends.
state('CrouchStartTurnL', tree('StartTurnAmount', None, [(f'Crouch_Loco_Walk_Start_Turn_L{d}_Lfoot', k + 1) for k, d in enumerate(('045', '090', '135', '180'))]),
      False, ['Crouch', 'Start', 'StartTurn'], (-520, 400), distance=start)
state('CrouchStartTurnR', tree('StartTurnAmount', None, [('Crouch_Loco_Walk_Start_Turn_R045_Lfoot', 1), ('Crouch_Loco_Walk_Start_Turn_R090_Lfoot', 2),
                                                        ('Crouch_Loco_Walk_Start_Turn_R135_Rfoot', 3), ('Crouch_Loco_Walk_Start_Turn_R180_Rfoot', 4)]),
      False, ['Crouch', 'Start', 'StartTurn'], (-520, 480), distance=start)
state('CrouchStop', tree('StopX', 'StopY', cardinal('Crouch_Loco_Walk', '_Stop')), False, ['Crouch', 'Stop'], (260, 560), speed=1.5, distance=stop)
state('CrouchPivot', tree('PivotX', 'PivotY', cardinal('Crouch_Loco_Walk', '_Pivot')), False, ['Crouch', 'Pivot'], (520, 560), distance=pivot)
state('CrouchFidget', tree('FidgetIndex', None, [(f'Crouch_Idle_0{i}' + s, i - 2) for i, s in
                                                 ((2, ''), (3, '_Look_Left'), (4, '_Look_Around'), (5, '_Look_Behind'), (6, '_Look_Behind'), (7, '_Look_Around'))]),
      False, ['Crouch', 'Fidget'], (-520, 560))

# --- transitions -------------------------------------------------------------------------------------------
IF, NOT = 'if', 'ifNot'
STAND_ONESHOTS = ['StartWalk', 'Start', 'StartRun', 'StartTurnL', 'StartTurnR', 'StopWalk', 'Stop', 'StopRun',
                  'PivotWalk', 'PivotJog', 'IdleStep', 'Fidget', 'Turn', 'Sprint']
CROUCH_ONESHOTS = ['CrouchStart', 'CrouchStartTurnL', 'CrouchStartTurnR', 'CrouchStop', 'CrouchPivot', 'CrouchFidget', 'CrouchTurn',
                   'CrouchDown', 'CrouchUp']

# The air, from anywhere on the ground. A jump is the input's: it cuts in at once.
for s in ['Locomotion', 'CrouchLoco'] + STAND_ONESHOTS + CROUCH_ONESHOTS:
    go(s, 'Jump', 0.08, [cond('Jump', IF)], offset=0.15)
    go(s, 'Fall', 0.25, [cond('Airborne', IF)])
go('Jump', 'Fall', 0.2, exit_time=0.85)
go('Jump', 'Land', 0.1, [cond('Grounded', IF)], exit_time=0.3)
go('Fall', 'Land', 0.1, [cond('Grounded', IF)])
go('Land', 'Locomotion', 0.2, [cond('Speed', 'greater', 0.5)], exit_time=0.15)
go('Land', 'Locomotion', 0.3, exit_time=0.5)

# Standing <-> crouched, from any standing state.
go('Locomotion', 'CrouchDown', 0.2, [cond('CrouchDown', IF)], offset=0.3)
# Crouching or standing still: the transition clip, tested before the plain fades below so its trigger is always used
# (left pending, a stand's trigger fired the next time the body crouched).
go('CrouchLoco', 'CrouchUp', 0.2, [cond('CrouchUp', IF)])
# 0.3 s: the eye rides the body's head down and up, so the fade is the view's crouch.
for s in ['Locomotion'] + STAND_ONESHOTS:
    go(s, 'CrouchLoco', 0.3, [cond('Crouched', IF)])
for s in ['CrouchLoco'] + CROUCH_ONESHOTS:
    if s not in ('CrouchDown', 'CrouchUp'):
        go(s, 'Locomotion', 0.3, [cond('Crouched', NOT)])
go('CrouchDown', 'Locomotion', 0.2, [cond('Crouched', NOT)])
go('CrouchDown', 'CrouchLoco', 0.2, [cond('Moving', IF)])
go('CrouchDown', 'CrouchLoco', 0.25, exit_time=0.92)
go('CrouchUp', 'CrouchLoco', 0.2, [cond('Crouched', IF)])
go('CrouchUp', 'Locomotion', 0.2, [cond('Moving', IF)])
go('CrouchUp', 'Locomotion', 0.3, exit_time=0.62)


def starts(src, crouched):
    # Turning starts first (they test the same trigger), then by gait.
    if crouched:
        go(src, 'CrouchStartTurnL', 0.12, [cond('Start', IF), cond('StartTurn', 'greater', 0.5)])
        go(src, 'CrouchStartTurnR', 0.12, [cond('Start', IF), cond('StartTurn', 'less', -0.5)])
        go(src, 'CrouchStart', 0.1, [cond('Start', IF)])
        return
    go(src, 'StartTurnL', 0.12, [cond('Start', IF), cond('StartTurn', 'greater', 0.5)])
    go(src, 'StartTurnR', 0.12, [cond('Start', IF), cond('StartTurn', 'less', -0.5)])
    go(src, 'StartRun', 0.1, [cond('Start', IF), cond('StartGait', 'greater', 1.5)])
    go(src, 'Start', 0.1, [cond('Start', IF), cond('StartGait', 'greater', 0.5)])
    go(src, 'StartWalk', 0.1, [cond('Start', IF)])


def stops(src, crouched):
    if crouched:
        go(src, 'CrouchStop', 0.15, [cond('Stop', IF)])
        return
    go(src, 'StopRun', 0.15, [cond('StopRun', IF)])
    go(src, 'Stop', 0.15, [cond('Stop', IF), cond('StopGait', 'greater', 0.5)])
    go(src, 'StopWalk', 0.15, [cond('Stop', IF)])


def pivots(src, crouched):
    if crouched:
        go(src, 'CrouchPivot', 0.15, [cond('Pivot', IF)])
        return
    go(src, 'PivotJog', 0.15, [cond('Pivot', IF), cond('PivotGait', 'greater', 0.5)])
    go(src, 'PivotWalk', 0.15, [cond('Pivot', IF)])


# Standing gait.
go('Locomotion', 'Turn', 0.15, [cond('Turning', IF)])
go('Turn', 'Locomotion', 0.25, [cond('Turning', NOT)])
starts('Locomotion', False)
stops('Locomotion', False)
pivots('Locomotion', False)
go('Locomotion', 'Sprint', 0.3, [cond('Sprint', IF), cond('MoveY', 'greater', 3.9)])
go('Locomotion', 'IdleStep', 0.15, [cond('Step', IF)])
go('Locomotion', 'Fidget', 0.45, [cond('Fidget', IF)])
# A start hands over to the loop once its push-off is done, or straight away when the input lets go, turns
# around (a pivot) or taps (a step).
for s in ('StartWalk', 'Start', 'StartRun', 'StartTurnL', 'StartTurnR'):
    go(s, 'IdleStep', 0.15, [cond('Step', IF)])
    stops(s, False)
    pivots(s, False)
    go(s, 'Locomotion', 0.15, [cond('Moving', NOT)])
    go(s, 'Sprint', 0.3, [cond('Sprint', IF), cond('MoveY', 'greater', 3.9)], exit_time=0.5)
    go(s, 'Locomotion', 0.25, exit_time=0.62 if 'Turn' in s else 0.5)
# A stop gives way to moving again at once (the input leads); else it settles into the idle.
for s in ('StopWalk', 'Stop', 'StopRun'):
    go(s, 'Locomotion', 0.2, [cond('Moving', IF)])
    go(s, 'Locomotion', 0.3, exit_time=0.9)
for s in ('PivotWalk', 'PivotJog'):
    stops(s, False)
    go(s, 'Locomotion', 0.25, [cond('Moving', NOT)])
    go(s, 'Locomotion', 0.25, exit_time=0.72)
go('Sprint', 'StopRun', 0.15, [cond('StopRun', IF)])
go('Sprint', 'Locomotion', 0.25, [cond('Sprint', NOT)])
go('Sprint', 'Locomotion', 0.25, [cond('MoveY', 'less', 3.6)])
# A step or a fidget is idling: moving off starts from it as from the idle.
starts('IdleStep', False)
go('IdleStep', 'Locomotion', 0.15, [cond('Moving', IF)])
go('IdleStep', 'Locomotion', 0.3, exit_time=0.85)
starts('Fidget', False)
go('Fidget', 'Locomotion', 0.2, [cond('Moving', IF)])
go('Fidget', 'Turn', 0.2, [cond('Turning', IF)])
go('Fidget', 'Locomotion', 0.45, exit_time=0.9)

# Crouched gait.
go('CrouchLoco', 'CrouchTurn', 0.15, [cond('Turning', IF)])
go('CrouchTurn', 'CrouchLoco', 0.25, [cond('Turning', NOT)])
starts('CrouchLoco', True)
stops('CrouchLoco', True)
pivots('CrouchLoco', True)
go('CrouchLoco', 'CrouchFidget', 0.45, [cond('Fidget', IF)])
for s in ('CrouchStart', 'CrouchStartTurnL', 'CrouchStartTurnR'):
    stops(s, True)
    pivots(s, True)
    go(s, 'CrouchLoco', 0.15, [cond('Moving', NOT)])
    go(s, 'CrouchLoco', 0.25, exit_time=0.62 if 'Turn' in s else 0.45)
go('CrouchStop', 'CrouchLoco', 0.2, [cond('Moving', IF)])
go('CrouchStop', 'CrouchLoco', 0.3, exit_time=0.92)
stops('CrouchPivot', True)
go('CrouchPivot', 'CrouchLoco', 0.25, [cond('Moving', NOT)])
go('CrouchPivot', 'CrouchLoco', 0.25, exit_time=0.72)
starts('CrouchFidget', True)
go('CrouchFidget', 'CrouchLoco', 0.2, [cond('Moving', IF)])
go('CrouchFidget', 'CrouchTurn', 0.2, [cond('Turning', IF)])
go('CrouchFidget', 'CrouchLoco', 0.45, exit_time=0.9)

layer = {'name': 'Base Layer', 'weight': 1.0, 'blending': 'override', 'defaultState': 'Locomotion',
         'entryPosition': [-780, 0], 'anyPosition': [-780, -120], 'exitPosition': [780, 0],
         'states': states, 'transitions': transitions}
template = {'version': 2, 'parameters': params, 'tracks': ['main'], 'layers': [layer]}

# Every role must name a file of the pack. The pack has an older locomotion set (Locomotion) under the same
# names as the current one (Locomotion_V2): the current one wins, and the mirrored set is never used.
files = {}
for dirpath, _, names in os.walk(os.path.join(PROJECT, *CLIP_DIR.split('/'))):
    rel_dir = os.path.relpath(dirpath, PROJECT).replace('\\', '/')
    if 'Locomotion_Mirror' in rel_dir:
        continue
    for n in names:
        if n.lower().endswith('.fbx') and n.startswith('AM_'):
            role, path = n[3:-4].lower(), rel_dir + '/' + n
            if role not in files or ('Locomotion_V2' in path and 'Locomotion_V2' not in files[role]):
                files[role] = path


def resolve(node):
    if isinstance(node, dict):
        out = {}
        for k, v in node.items():
            if k == 'clip':
                path = files.get(v.lower())
                if not path:
                    sys.exit(f'no clip for role {v}')
                out[k] = path
            else:
                out[k] = resolve(v)
        return out
    if isinstance(node, list):
        return [resolve(x) for x in node]
    return node


def write(path, data):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=2)
        f.write('\n')


write(os.path.join(CTRL_DIR, 'fps_body_locomotion.template.json'), template)
write(os.path.join(CTRL_DIR, 'fps_body_locomotion.controller'), resolve(template))
print(f'{len(states)} states, {len(params)} parameters, {len(transitions)} transitions')
