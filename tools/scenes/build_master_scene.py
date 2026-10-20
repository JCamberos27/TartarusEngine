# The Master Scene: the project's night sandbox. A 200 x 200 m dark slate yard (soft grid: 10 cm / 1 m / 5 m), each
# course in its own colour (range amber, traversal teal, architecture steel blue, NPC bay rose, hub graphite),
# real-world measurement courses labelled with World Text, a big moon over a physical night sky, shadowed work lights,
# a night ambience bed under an outdoor reverb zone, wind and work-light hum emitters, a naked (underpants) player
# with the AKS-74U and Remington 870, and bare-body training dummies.
#
#   python tools/scenes/build_master_scene.py [<repo>]
#
# Writes project/scenes/Master Scene.json (no longer committed: the Apartment Hallway is the startup scene) (+ .meta, its guid kept across rebuilds). Render settings and the player's
# rig come from scenes/Arena.json; the grid textures from tools/assets/gen_proto_grid.py.
#
# Layout (X east, -Z north, metres; the player spawns at the hub facing north):
#   hub (0, 0)            spawn, compass, scene sign
#   firing range (N)      a 12 m lane to 100 m: targets at 5 / 10 / 25 / 50 / 100 m, a material wall
#   traversal (NE)        steps 10-100 cm, ramps 15-60 deg, jump gaps 1-5 m, mantle ledges, crouch tunnels
#   architecture (W)      doors, corridors, ceilings, cover, a standard stair
#   lighting bay (S)      the engine's test materials under a spot and fills
#   NPC bay (E)           bare-body training dummies
# Every module has a 1.80 m height pole (the UE5 Manny's height) ticked every 10 cm.
import json
import math
import os
import secrets
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scene_kit import Scene, euler_quat  # noqa: E402

MANNY_M = 1.80               # the UE5 mannequin (Manny) is ~180 cm; the Quantum rig is authored to match
NORTH_OFFSET = 50.0         # degrees the sky is turned: the moon over the firing range
GRID_SCALE = 0.2             # triplanar: one 5 m texture tile per 5 m
LABEL = dict(Brightness=1.5, Outline=0.12, FadeStart=25.0, CullDistance=35.0)

# The palette: base colours multiply the neutral grid (mid-grey), so these are darker than they read on screen. Dark,
# desaturated bodies; each course's accent glows faintly in its grid lines and colours its signs.
PALETTE = {
    'ground':    dict(tint=(0.085, 0.092, 0.105), glow=0.10, glow_color=(0.35, 0.45, 0.6)),
    'graphite':  dict(tint=(0.22, 0.225, 0.24), glow=0.0),
    'range':     dict(tint=(0.95, 0.40, 0.10), glow=0.30),
    'traversal': dict(tint=(0.14, 0.55, 0.50), glow=0.30),
    'arch':      dict(tint=(0.20, 0.32, 0.60), glow=0.30),
    'npc':       dict(tint=(0.75, 0.18, 0.30), glow=0.30),
    'pole':      dict(tint=(1.2, 1.17, 1.1), glow=0.0),
    'tick':      dict(tint=(0.05, 0.05, 0.055), glow=0.0),
}
TEXT = (1.0, 0.95, 0.88)                         # warm off-white: every measurement
ACCENT = {'hub': (1.0, 0.95, 0.88), 'range': (1.0, 0.62, 0.25), 'traversal': (0.35, 0.95, 0.85),
          'arch': (0.55, 0.72, 1.0), 'npc': (1.0, 0.45, 0.62), 'lighting': (0.85, 0.85, 0.9)}
WORK_LIGHT_K = 3000.0                            # warm sodium-ish work lights against the cool moon


def ref(repo, path):
    with open(os.path.join(repo, 'project', path + '.meta'), encoding='utf-8') as f:
        return {'path': path, 'pathGuid': json.load(f)['guid']}


def grid(repo, tint=(1.0, 1.0, 1.0), roughness=0.85, metallic=0.0, glow=0.0, glow_color=None):
    """The neutral grid tinted by `tint`; its 1 m / 5 m lines glow faintly in `glow_color` (the tint by default)."""
    gc = glow_color or tint
    return {'albedoMap': ref(repo, 'assets/Materials/Prototype/proto_grid_neutral.png'), 'aoMap': '', 'baseColor': list(tint),
            'emissiveColor': [min(c, 1.0) for c in gc],
            'emissiveMap': ref(repo, 'assets/Materials/Prototype/proto_grid_neutral_emissive.png'),
            'emissiveStrength': glow, 'factorsScaleMaps': True, 'metallic': metallic, 'metallicMap': '', 'metallicRoughnessMap': '',
            'normalMap': '', 'roughness': roughness, 'roughnessMap': '', 'triplanar': True, 'triplanarScale': GRID_SCALE}


def metres(v):
    """A measurement as the labels print it: centimetres under a metre, else metres to two places."""
    return f'{v * 100:.0f} cm' if v < 1.0 - 1e-6 else f'{v:.2f} m'


class Master(Scene):
    def __init__(self, base, repo):
        super().__init__(base)
        self.repo = repo
        self.mats = {k: grid(repo, **v) for k, v in PALETTE.items()}
        self.mat = self.mats['graphite']
        self.accent = ACCENT['hub']

    def module(self, name):
        """Subsequent blocks and signs in this course's colours."""
        self.mat = self.mats.get(name, self.mats['graphite'])
        self.accent = ACCENT.get(name, TEXT)

    def label(self, text, pos, size=0.3, face='Fixed', yaw=0.0, pitch=0.0, parent=-1, align='Center', **kw):
        wt = {'Text': text, 'Size': size, 'Facing': face, 'Align': align, 'Color': list(TEXT),
              'Brightness': LABEL['Brightness'], 'Outline': LABEL['Outline'], 'Fade Start': LABEL['FadeStart'],
              'Cull Distance': LABEL['CullDistance'], 'On Top': False}
        wt.update(kw)
        return self.empty(f'Label {text.splitlines()[0]}', parent, pos, yaw=yaw, pitch=pitch, **{'World Text': wt})

    def floor_label(self, text, x, z, size=0.35, yaw=0.0, **kw):
        """Painted on the ground, read from the south (the player's side): lies flat, 3 mm up."""
        return self.label(text, (x, 0.003, z), size, 'Fixed', yaw=yaw, pitch=-90.0, **kw)

    def sign(self, text, x, y, z, size=0.3, **kw):
        """Stands upright and turns to face the viewer."""
        return self.label(text, (x, y, z), size, 'Upright', **kw)

    def header(self, text, x, y, z, size=0.4, **kw):
        """A course's name: its accent colour, a little brighter."""
        kw.setdefault('Color', list(self.accent))
        kw.setdefault('Brightness', 1.7)
        return self.sign(text, x, y, z, size, **kw)

    def block(self, name, x0, x1, y0, y1, z0, z1, parent=-1, material=None, **extra):
        return self.box(name, parent, ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), (x1 - x0, y1 - y0, z1 - z0),
                        material or self.mat, **extra)

    def pole(self, x, z, parent=-1):
        """The Manny-height reference: a 1.80 m post ticked every 10 cm (long ticks each 50 cm), labelled."""
        g = self.empty('Height Pole 1.80 m', parent, (x, 0, z))
        self.box('Pole', g, (0, MANNY_M / 2, 0), (0.04, MANNY_M, 0.04), self.mats['pole'], collider=False)
        for i in range(1, 19):
            long_tick = i % 5 == 0
            w = 0.16 if long_tick else 0.09
            self.box(f'Tick {i * 10} cm', g, (0.02 + w / 2, i * 0.1, 0), (w, 0.008, 0.008), self.mats['tick'], collider=False)
        self.label(metres(MANNY_M), (0.12, MANNY_M + 0.12, 0), 0.12, 'Upright', parent=g)
        self.label('1.00 m', (0.26, 1.0, 0), 0.08, 'Upright', parent=g, align='Left')
        return g

    def work_light(self, name, pos, aim_pitch=-60.0, yaw=0.0, shadows=True, intensity=40.0, rng=40.0, spot=40.0):
        """A shadowed warm work light on a graphite mast, with its electrical hum."""
        x, y, z = pos
        self.block(f'{name} Mast', x - 0.08, x + 0.08, 0.0, y - 0.15, z - 0.08, z + 0.08, material=self.mats['graphite'])
        lid = self.light(name, -1, (x, y, z), intensity=intensity * 1.6, rng=rng, shadows=shadows, kelvin=WORK_LIGHT_K,
                         spot=spot,
                         pitch=aim_pitch, yaw=yaw, softness=1.5)
        self.empty(f'{name} Hum', lid, (0, 0, 0), **{'Audio Source': {
            'Clip': 'assets/Audio/Ambience/worklight_hum_1.wav', 'Volume': 0.35, 'Loop': True, 'Play On Start': True,
            'Output': 'Ambient', '3D Sound': True, 'Volume Rolloff': 'Linear', 'Min Distance': 1.0, 'Max Distance': 9.0,
            'Doppler Level': 0.0}})
        return lid


def build(repo):
    with open(os.path.join(repo, 'project', 'scenes', 'Arena.json'), encoding='utf-8') as f:
        arena = json.load(f)
    s = Master(arena, repo)
    d = s.data

    # --- Look: physical night sky (never an HDRI or a flat colour), a big full moon, no clouds ---------------------
    # Full moon (phase 0.5) at 19:00 on day 355 (midwinter: the full moon rides high): offset = 0.5 - frac((day + h/24)
    # / 29.53). Hour angle -75 deg puts it ~25 deg up; North Offset turns the sky so it hangs over the range, in view from
    # the spawn. Really big on purpose: 20x the real moon's apparent size.
    day, hours = 355.0, 19.0
    phase_offset = (0.5 - ((day + hours / 24.0) / 29.53 % 1.0)) % 1.0
    d['skySource'] = 2
    d['sky'].update({'timeOfDay': True, 'timeOfDayHours': hours, 'dayOfYear': day, 'latitude': 38.0, 'northOffset': NORTH_OFFSET,
                     'moon': True, 'moonDiscScale': 20.0, 'moonBrightness': 0.012, 'moonPhaseOffset': round(phase_offset, 4),
                     'starsBrightness': 1.6, 'nightGlow': 0.02, 'nightBrightness': 5.0, 'clouds': False,
                     'cloudShadows': False, 'cirrusCoverage': 0.0, 'animateInEditor': False, 'dayLengthMinutes': 0.0})
    d.update({'exposureEV': 1.0, 'autoExposure': False, 'tonemapOperator': 1,
              'bloomEnabled': True, 'bloomThreshold': 1.2, 'bloomKnee': 0.5, 'bloomIntensity': 0.1,
              'fogEnabled': True, 'fogMode': 2, 'fogDensity': 0.006, 'fogColor': [0.05, 0.06, 0.08],
              'fogBaseHeight': 0.0, 'fogHeightFalloff': 0.08,
              'ssaoEnabled': True, 'shadowsEnabled': True, 'shadowResolution': 2048, 'shadowCascades': 2,
              'shadowDistance': 120.0, 'maxSpotShadows': 4, 'maxPointShadows': 0,
              'vignetteIntensity': 0.15, 'filmGrain': 0.0, 'motionBlur': False, 'depthOfField': False,
              'chromaticAberration': 0.0, 'gradeContrast': 0.0, 'gradeSaturation': 0.0})
    d['skyHdriPath'] = ''
    d.pop('skyHdriGuid', None)

    # The scene's first directional light: the time of day drives it (the moon by night).
    s.empty('Moon', -1, (0, 40, 0), pitch=-37.0, yaw=-110.0, Light={
        'Type': 'Directional', 'Color': [0.78, 0.86, 1.0], 'Intensity': 0.6, 'Range': 200.0, 'Spot Angle': 35.0,
        'Angular Size': 0.53, 'Cast Shadows': True, 'Shadow Bias': 1.0, 'Shadow Resolution': 'Follow global',
        'Shadow Update Mode': 'Dynamic', 'ColorTempK': 0.0, 'Culling Mask': -1})

    # --- Ground: one 200 x 200 m slab (the grid is world-space, so every box lines up with it) ----------------------
    s.block('Ground (concrete)', -100, 100, -0.2, 0.0, -100, 100, material=s.mats['ground'])

    # --- Audio: the night bed under an outdoor reverb, wind at the edges, everyone barefoot -------------------------
    s.empty('Night Reverb Zone', -1, (0, 15, 0), **{'Reverb Zone': {
        'Enabled': True, 'Shape': 'Box', 'Extents': [100.0, 30.0, 100.0], 'Radius': 6.0, 'Priority': 0, 'Fade Distance': 4.0,
        'Tail Class': 'Outdoor Open', 'Tail Gain': 1.0, 'Reverb Mode': 'Class Default', 'Ambience': 'snd.amb.night_open',
        'Ambience Volume': 1.0}})
    for i, (x, z) in enumerate([(-70, -60), (75, -30), (-40, 70)]):
        s.empty(f'Wind Emitter {i + 1}', -1, (x, 6, z), **{'Audio Source': {
            'Clip': f'assets/Audio/Ambience/night_wind_{i % 2 + 1}.wav', 'Volume': 0.7, 'Loop': True, 'Play On Start': True,
            'Output': 'Ambient', '3D Sound': True, 'Volume Rolloff': 'Logarithmic', 'Min Distance': 8.0,
            'Max Distance': 60.0, 'Doppler Level': 0.0}})
    s.empty('Foley', -1, (0, 0, 0), **{'Foley Audio': {'Footwear': 'Barefoot', 'NPC Footwear': 'Barefoot'}})

    hub(s)
    firing_range(s)
    traversal(s)
    architecture(s)
    lighting_bay(s, repo)
    npc_bay(s)
    player(s, arena, repo)
    return s


def hub(s):
    s.module('hub')
    s.header('MASTER SCENE', 0, 3.2, -6, 0.6)
    s.sign('night sandbox  ·  1 m grid  ·  Manny 1.80 m', 0, 2.55, -6, 0.3)
    # Compass and axes, painted around the spawn.
    s.floor_label('N  (-Z)', 0, -3.0, 0.5)
    s.floor_label('S  (+Z)', 0, 9.0, 0.5)
    s.floor_label('E  (+X)', 4.5, 3.0, 0.5)
    s.floor_label('W  (-X)', -4.5, 3.0, 0.5)
    s.floor_label('FIRING RANGE', 0, -8.0, 0.4, Color=list(ACCENT['range']))
    s.floor_label('TRAVERSAL', 6.5, -6.0, 0.4, Color=list(ACCENT['traversal']))
    s.floor_label('ARCHITECTURE', -6.5, -6.0, 0.4, Color=list(ACCENT['arch']))
    s.floor_label('LIGHTING BAY', 0, 12.0, 0.4, Color=list(ACCENT['lighting']))
    s.floor_label('NPC BAY', 8.0, 3.0, 0.4, Color=list(ACCENT['npc']))
    s.pole(2.0, 2.0)
    s.work_light('Hub Light', (-5.0, 6.0, 4.0), aim_pitch=-55.0, yaw=-40.0, spot=50.0, intensity=35.0)


def firing_range(s):
    s.module('range')
    line = -10.0                                    # the firing line; distances are measured from it
    g = s.mats['graphite']
    s.block('Firing Line', -6.0, 6.0, 0.0, 0.03, line - 0.15, line + 0.15, collider=False)
    s.floor_label('FIRING LINE', 0, line + 0.8, 0.35, Color=list(s.accent))
    for x in (-6.25, 6.25):                         # low side berms down the lane
        s.block('Range Berm', x - 0.25, x + 0.25, 0.0, 1.2, line - 108.0, line, material=g)
    # Tall and wide enough that a round fired level from the spawn can't sail over it (a 3.5 deg shotgun pellet from the
    # hub clears 8 m by centimetres).
    s.block('Backstop (concrete)', -12, 12, 0.0, 10.0, line - 112.0, line - 109.0, material=g)
    for dist in (5, 10, 25, 50, 100):
        z = line - dist
        # A man-sized target (0.5 x 1.8 m, the Manny's height) on a stand, its distance above it and on the floor.
        s.block(f'Target {dist} m', -0.25, 0.25, 0.0, MANNY_M, z - 0.05, z + 0.05)
        s.sign(f'{dist} m', 0, MANNY_M + 0.45, z, 0.45 if dist < 50 else 0.9, **{'Fade Start': 120.0, 'Cull Distance': 140.0})
        s.floor_label(f'{dist} m', 3.5, z, 0.6)
        s.block(f'Distance Mark {dist} m', -6.0, 6.0, 0.0, 0.02, z - 0.04, z + 0.04, collider=False, material=g)
    # The material wall: what bullets do to each surface (names carry the surface for impacts and footsteps).
    wall = [('Concrete', (0.42, 0.42, 0.42), 0.9, 0.0), ('Wood', (0.55, 0.33, 0.16), 0.7, 0.0),
            ('Metal', (0.62, 0.64, 0.68), 0.35, 1.0), ('Glass', (0.22, 0.38, 0.48), 0.05, 0.0)]
    for i, (name, tint, rough, metal) in enumerate(wall):
        x0 = -20.0 + i * 2.6
        s.block(f'Material Wall {name.lower()}', x0, x0 + 2.0, 0.0, 2.5, line - 15.3, line - 15.0,
                material=grid(s.repo, tint, rough, metal))
        s.sign(f'{name.upper()}\n30 cm', x0 + 1.0, 2.9, line - 15.15, 0.22)
    s.header('FIRING RANGE', 0, 3.4, line - 1.0, 0.45)
    s.pole(-7.5, line - 2.0)
    s.work_light('Range Light', (7.5, 6.5, line + 2.0), aim_pitch=-30.0, yaw=20.0, spot=35.0, intensity=60.0, rng=60.0)


def traversal(s):
    s.module('traversal')
    x0, z0 = 16.0, -12.0
    # Steps: one block per height, 10 to 100 cm.
    for i in range(10):
        h = (i + 1) * 0.1
        x = x0 + i * 2.2
        s.block(f'Step {metres(h)}', x, x + 1.6, 0.0, h, z0 - 1.6, z0)
        s.floor_label(metres(h), x + 0.8, z0 + 0.6, 0.28)
    s.header('STEP HEIGHT', x0 + 10.0, 1.9, z0 - 0.8, 0.35)
    # Ramps rising to the north over a 6 m run.
    for i, deg in enumerate((15, 30, 45, 60)):
        p = math.radians(deg)
        run, width, t = 6.0, 3.0, 0.2
        length = run / math.cos(p)
        x = x0 + i * 5.0 + width / 2
        zc = z0 - 6.0 - run / 2
        yc = (length / 2) * math.sin(p) - (t / 2) * math.cos(p)
        ramp = s.box(f'Ramp {deg}°', -1, (x, yc, zc), (width, t, length), s.mat)
        ramp['rotation'] = euler_quat(0.0, deg)       # rises to the north (+pitch tips +Y toward +Z)
        rise = run * math.tan(p)
        s.floor_label(f'{math.degrees(math.atan2(rise, run)):.0f}°', x, z0 - 5.0, 0.4)
        s.floor_label(f'rise {metres(rise)}', x, z0 - 4.4, 0.18)
    s.header('RAMPS', x0 + 10.0, 2.0, z0 - 4.6, 0.35)
    # Jump gaps between 1 m high platforms.
    zg = z0 - 20.0
    for i, gap in enumerate((1.0, 2.0, 3.0, 4.0, 5.0)):
        x = x0 + i * 2.6
        s.block(f'Gap {gap:.0f} m Take-off', x, x + 2.0, 0.0, 1.0, zg, zg + 2.0)
        s.block(f'Gap {gap:.0f} m Landing', x, x + 2.0, 0.0, 1.0, zg - gap - 2.0, zg - gap)
        s.label(f'GAP {gap:.1f} m', (x + 1.0, 1.003, zg + 1.0), 0.22, pitch=-90.0)
    # Mantle ledges.
    zl = z0 - 34.0
    for i, h in enumerate((0.9, 1.2, 1.5, 2.0)):
        x = x0 + i * 4.0
        s.block(f'Ledge {metres(h)}', x, x + 3.0, 0.0, h, zl - 1.0, zl)
        s.floor_label(f'LEDGE {metres(h)}', x + 1.5, zl + 0.7, 0.25)
    # Crouch tunnels: two walls and a roof at the clear height.
    zt = z0 - 42.0
    for i, h in enumerate((1.0, 1.2)):
        x = x0 + i * 5.0
        s.block(f'Tunnel {metres(h)} Wall L', x, x + 0.3, 0.0, h + 0.3, zt - 6.0, zt)
        s.block(f'Tunnel {metres(h)} Wall R', x + 1.5, x + 1.8, 0.0, h + 0.3, zt - 6.0, zt)
        s.block(f'Tunnel {metres(h)} Roof', x, x + 1.8, h, h + 0.3, zt - 6.0, zt)
        s.floor_label(f'CLEAR {metres(h)}', x + 0.9, zt + 0.7, 0.22)
    s.pole(x0 - 2.0, z0 - 1.0)
    s.work_light('Traversal Light', (x0 - 3.0, 7.0, z0 - 20.0), aim_pitch=-35.0, yaw=-90.0, spot=55.0, intensity=60.0,
                 rng=50.0)


def architecture(s):
    s.module('arch')
    x0, z0 = -16.0, -12.0
    s.header('ARCHITECTURE', x0 - 6.0, 3.8, z0 + 1.5, 0.45)
    wall_h, t = 3.0, 0.2
    # Doors (2.1 m tall) cut into a wall: two jambs and a lintel each.
    xs = x0
    for w in (0.9, 1.2, 2.0):
        s.block(f'Door {metres(w)} Jamb R', xs - 1.0, xs, 0.0, wall_h, z0 - t, z0)
        s.block(f'Door {metres(w)} Lintel', xs - 1.0 - w, xs - 1.0, 2.1, wall_h, z0 - t, z0)
        s.floor_label(f'DOOR {metres(w)} × 2.10 m', xs - 1.0 - w / 2, z0 + 0.8, 0.2)
        xs -= 1.0 + w
    s.block('Door Wall End', xs - 1.0, xs, 0.0, wall_h, z0 - t, z0)
    # Corridors, 10 m long.
    zc = z0 - 6.0
    xs = x0
    for w in (1.2, 1.8, 2.4):
        s.block(f'Corridor {metres(w)} Wall E', xs - t, xs, 0.0, wall_h, zc - 10.0, zc)
        s.block(f'Corridor {metres(w)} Wall W', xs - t - w - t, xs - t - w, 0.0, wall_h, zc - 10.0, zc)
        s.floor_label(f'CORRIDOR {metres(w)}', xs - t - w / 2, zc + 0.8, 0.2)
        xs -= w + 2 * t + 1.5
    # Ceilings: 4 x 4 m pavilions on four posts.
    zp = z0 - 22.0
    for i, h in enumerate((2.4, 3.0, 4.0)):
        xc = x0 - 2.5 - i * 6.0
        for dx in (-1.9, 1.9):
            for dz in (-1.9, 1.9):
                s.block(f'Ceiling {metres(h)} Post', xc + dx - 0.1, xc + dx + 0.1, 0.0, h, zp + dz - 0.1, zp + dz + 0.1)
        s.block(f'Ceiling {metres(h)} Slab', xc - 2.0, xc + 2.0, h, h + 0.2, zp - 2.0, zp + 2.0)
        s.floor_label(f'CEILING {metres(h)}', xc, zp, 0.3)
    # Cover: half (crouched) and full (standing) height.
    zv = z0 - 30.0
    for i, h in enumerate((0.6, 1.2)):
        xc = x0 - 3.0 - i * 4.0
        s.block(f'Cover {metres(h)}', xc - 1.0, xc + 1.0, 0.0, h, zv - 0.5, zv)
        s.floor_label(f'{"HALF" if h < 1 else "FULL"} COVER {metres(h)}', xc, zv + 0.8, 0.2)
    # A standard stair: 18 cm risers, 28 cm treads, to a 1.80 m landing.
    zs, rise, tread = z0 - 36.0, 0.18, 0.28
    xs0 = x0 - 4.0
    n = round(MANNY_M / rise)
    for i in range(n):
        s.block(f'Stair Step {i + 1}', xs0 - 1.2, xs0, 0.0, (i + 1) * rise, zs - (i + 1) * tread, zs - i * tread)
    s.block('Stair Landing', xs0 - 1.2, xs0, 0.0, n * rise, zs - n * tread - 2.0, zs - n * tread)
    s.floor_label(f'STAIR {metres(rise)} / {metres(tread)}', xs0 - 0.6, zs + 0.8, 0.2)
    s.pole(x0 + 2.0, z0 + 1.0)
    s.work_light('Architecture Light', (x0 + 4.0, 7.0, z0 - 18.0), aim_pitch=-35.0, yaw=90.0, spot=55.0, intensity=60.0,
                 rng=50.0)


def lighting_bay(s, repo):
    s.module('lighting')
    z0 = 16.0
    s.block('Lighting Bay Floor', -10, 10, 0.0, 0.02, z0, z0 + 10, collider=False)
    s.header('LIGHTING BAY', 0, 2.4, z0 + 9.0, 0.4)
    mats = ['mat_baseline', 'mat_clearcoat', 'mat_aniso', 'mat_sheen', 'mat_probes']
    for i, m in enumerate(mats):
        x = -8.0 + i * 4.0
        path = f'assets/Materials/Test/{m}.mat'
        mr = ref(repo, path)
        s.models.append(dict(s._common(f'Sphere {m}', -1, (x, 0.6, z0 + 5.0), 0.0, (1.2, 1.2, 1.2)),
                             path='primitive://sphere#s30', materials=[{'guid': mr['pathGuid'], 'path': path}]))
        s.floor_label(m.replace('mat_', '').upper(), x, z0 + 6.6, 0.25, yaw=180.0)
    s.pole(-11.0, z0 + 5.0)
    # Unshadowed: the four shadowed spots are spent on the courses.
    s.light('Lighting Bay Key', -1, (0, 6.0, z0 + 1.0), intensity=40.0, rng=20.0, shadows=False, kelvin=5600.0, spot=45.0,
            pitch=-60.0, yaw=180.0)
    s.light('Lighting Bay Fill', -1, (6.0, 2.5, z0 + 8.0), intensity=6.0, rng=10.0, kelvin=7500.0)


def npc_bay(s):
    s.module('npc')
    x0, z0 = 16.0, -2.0
    s.block('NPC Pad', x0, x0 + 12, 0.0, 0.02, z0, z0 + 10, collider=False)
    s.header('NPC BAY  ·  training dummies', x0 + 6, 2.6, z0 + 1.0, 0.3)
    for i in range(3):
        s.empty(f'Dummy Spawn {i + 1}', -1, (x0 + 3.0 + i * 3.0, 0.0, z0 + 4.0), yaw=90.0, **{'NPC Spawn': {
            'Brain': 'Training Dummy', 'Outfit Seed': i + 1, 'Skill': 0.5, 'Squad': 1, 'Weapon': 'AKS-74U', 'Bare': True}})
    s.pole(x0 - 1.0, z0 + 4.0)
    s.light('NPC Bay Fill', -1, (x0 + 6.0, 4.0, z0 + 8.0), intensity=10.0, rng=14.0, kelvin=5600.0)


def player(s, arena, repo):
    """Arena's player (controller, weapons, first-person body) at the hub, stripped to the bare body + underpants."""
    ents = {e['id']: e for k in ('empties', 'models') for e in arena.get(k, [])}
    spawn = next(e for e in arena['empties'] if 'First Person Controller' in e)
    body = next(e for e in arena['empties'] if e.get('parentId') == spawn['id'] and 'First Person Body' in e)
    keep_slots = {'Torso', 'Head', 'Arms'}
    pieces = [e for e in arena['models'] if e.get('parentId') == body['id'] and e.get('Outfit Piece', {}).get('Slot') in keep_slots]
    anim = pieces[0]['Animator Controller']
    sid = s.empty('Player Spawn', -1, (0.0, 0.0, 6.0), **{k: v for k, v in spawn.items() if k[0].isupper()})
    bid = s.empty('Player Body', sid, tuple(body['position']), **{k: v for k, v in body.items() if k[0].isupper()})
    for p in pieces:
        e = s._common(p['name'], bid, tuple(p['position']), 0.0)
        e['rotation'] = p['rotation']
        e.update({k: v for k, v in p.items() if k in ('path', 'pathGuid', 'materials', 'Outfit Piece', 'Animator Controller')})
        s.models.append(e)
    body_mat = {'guid': ref(repo, 'assets/Characters/Quantum/Materials/Characters/M_Quantum_Body.mat')['pathGuid'],
                'path': 'assets/Characters/Quantum/Materials/Characters/M_Quantum_Body.mat'}
    for slot, model in (('Legs', 'Quantum_Legs'), ('Feet', 'Quantum_Feet'), ('UnderPants', 'Quantum_UnderPants')):
        path = f'assets/Characters/Quantum/Models/European/{model}.fbx'
        r = ref(repo, path)
        e = s._common(slot, bid, (0.0, 0.0, 0.0), 0.0)
        e.update({'path': path, 'pathGuid': r['pathGuid'], 'materials': [body_mat], 'Animator Controller': anim,
                  'Outfit Piece': {'Flags': 1, 'Item': {'path': path, 'pathGuid': r['pathGuid']}, 'Slot': slot}})
        s.models.append(e)


def write_meta(path):
    meta = path + '.meta'
    if os.path.exists(meta):
        return  # keep the guid: settings.json and the build list point at it
    with open(meta, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'guid': secrets.token_hex(8), 'metaVersion': 1, 'type': 'scene'}, f, indent=2)
        f.write('\n')


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    s = build(repo)
    out = os.path.join(repo, 'project', 'scenes', 'Master Scene.json')
    s.write(out)
    write_meta(out)
    labels = sum(1 for e in s.empties if 'World Text' in e)
    print(f'{out}: {len(s.boxes)} boxes, {len(s.models)} models, {len(s.empties)} empties ({labels} labels)')


if __name__ == '__main__':
    main()
