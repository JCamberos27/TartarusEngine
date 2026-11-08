# Generates tests/perf-scenes/perf_stress.json: the renderer under a known load, for --perf-bench. Engine primitives
# and committed test assets only (prototype grids, the test decal, the transmission material), so it runs in any
# clone and on CI. A scene Camera holds the view fixed in every phase (edit, play, play maximized).
# Load: a 10 x 10 block of pillars plus 300 instanced crates, sun with 4 cascades, 16 shadowed spots, 8 shadowed
# points, 160 unshadowed points, 24 casters spinning in Play (so the spot / point caches redraw), 300 wall decals +
# 100 floor decals, 6 particle systems, 8 glass spheres, a reflection probe, physical sky with volumetric clouds and
# cloud shadows, SSAO, bloom, MSAA 4x and the froxel fog.
# No NPCs: their models live on the asset Drive. The Apartment Hallway bench covers skinning.
# Deterministic: same script, same file.
# Usage: python tools/gen_perf_stress_scene.py [repo root]
import json
import math
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
out = os.path.join(root, 'tests', 'perf-scenes', 'perf_stress.json')

ents = {'boxes': [], 'empties': [], 'models': []}
_next = [0]


def nid():
    _next[0] += 1
    return _next[0] - 1


def quat_euler(pitch=0.0, yaw=0.0, roll=0.0):
    # Degrees, applied yaw (Y) * pitch (X) * roll (Z); returns [x, y, z, w].
    p, y, r = (math.radians(a) * 0.5 for a in (pitch, yaw, roll))
    cy, sy, cp, sp, cr, sr = math.cos(y), math.sin(y), math.cos(p), math.sin(p), math.cos(r), math.sin(r)
    w = cy * cp * cr + sy * sp * sr
    x = cy * sp * cr + sy * cp * sr
    yy = sy * cp * cr - cy * sp * sr
    z = cy * cp * sr - sy * sp * cr
    return [round(x, 6), round(yy, 6), round(z, 6), round(w, 6)]


TEX = {'light': ('assets/Materials/Prototype/proto_grid_light.png', 'c90849c8fc6c88b0'),
       'dark': ('assets/Materials/Prototype/proto_grid_dark.png', '2b60657ce9aa4555'),
       'orange': ('assets/Materials/Prototype/proto_grid_orange.png', 'c389fb6bc574bb89')}


def mat(tex, rough=0.85, color=(1, 1, 1), metallic=0.0):
    path, guid = TEX[tex]
    return [{'embedded': {'albedoMap': {'path': path, 'pathGuid': guid}, 'aoMap': '', 'baseColor': list(color),
                          'emissiveColor': [0, 0, 0], 'emissiveMap': '', 'emissiveStrength': 1.0,
                          'factorsScaleMaps': True, 'metallic': metallic, 'metallicMap': '',
                          'metallicRoughnessMap': '', 'normalMap': '', 'roughness': rough, 'roughnessMap': '',
                          'triplanar': True, 'triplanarScale': 0.25}}]


GROUND, WALL, ACCENT = mat('light', 0.9), mat('dark'), mat('orange', 0.75)
CRATE = mat('orange', 0.8, (0.85, 0.7, 0.55))
METAL = mat('light', 0.35, (0.8, 0.8, 0.82), 1.0)
GLASS = [{'path': 'assets/Materials/Test/mat_transmission.mat'}]


def base(name, parent, pos, rot=(0.0, 0.0, 0.0), scale=(1, 1, 1)):
    i = nid()
    return {'id': i, 'name': name, 'order': i, 'parentId': parent, 'position': [round(v, 4) for v in pos],
            'rotation': quat_euler(*rot), 'scale': [round(v, 4) for v in scale]}


def group(name, parent=-1):
    e = base(name, parent, (0, 0, 0))
    ents['empties'].append(e)
    return e['id']


def box(name, center, size, parent, m=WALL, yaw=0.0):
    e = base(name, parent, center, (0.0, yaw, 0.0), size)
    e.update({'collider': {'friction': 0.8, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0], 'materials': m})
    ents['boxes'].append(e)
    return e['id']


def light(name, parent, pos, rot, kind, color, intensity, rng, shadows, angle=35.0):
    e = base(name, parent, pos, rot)
    e['Light'] = {'Angular Size': 1.0, 'Cast Shadows': shadows, 'Color': list(color), 'ColorTempK': 0.0,
                  'Culling Mask': -1, 'Intensity': intensity, 'Range': rng, 'Shadow Bias': 1.0,
                  'Shadow Near Plane': 0.1 if kind != 'Directional' else 0.05, 'Shadow Normal Bias': 1.0,
                  'Shadow Resolution': 'Follow global', 'Shadow Softness': 0.35, 'Shadow Update Mode': 'Dynamic',
                  'Spot Angle': angle, 'Type': kind}
    ents['empties'].append(e)


def decal(name, parent, pos, rot, size, seed):
    e = base(name, parent, pos, rot)
    e['Decal'] = {'Asset': {'path': 'assets/Test/Decals/DA_Test_Grid.decal'}, 'Size': list(size), 'Seed': seed,
                  'Sort Order': 0, 'Affect Static': True, 'Affect Dynamic': True, 'Rotation Jitter': 25.0,
                  'Scale Jitter': 0.15, 'Random Flip': True}
    ents['empties'].append(e)


# --- geometry --------------------------------------------------------------------------------------------------------
env = group('Environment')
box('Ground', (0, -0.1, 0), (90, 0.2, 90), env, GROUND)

pillars = group('Pillars')
SPACING = 6.0
for i in range(10):
    for j in range(10):
        h = 1.5 + ((i * 7 + j * 3) % 5) * 1.1
        x, z = (i - 4.5) * SPACING, (j - 4.5) * SPACING - 6.0
        m = (WALL, ACCENT, METAL)[(i + 2 * j) % 3]
        box(f'Pillar {i}-{j}', (x, h * 0.5, z), (1.6, h, 1.6), pillars, m, yaw=(i * 13 + j * 29) % 90)

crates = group('Crates')
for k in range(300):
    i, j = k % 20, k // 20
    x, z = (i - 9.5) * 3.0 + 1.5, (j - 7.0) * 3.6 - 4.0
    lv = (k * 7) % 3 == 0
    box(f'Crate {k + 1}', (x, 0.4 + (0.8 if lv else 0.0), z), (0.8, 0.8, 0.8), crates, CRATE, yaw=(k * 37) % 90)

# The decal wall: across the back of the block, facing the camera (+Z).
WALL_Z = -40.0
box('Decal Wall', (0, 4.0, WALL_Z), (48, 8, 0.4), env, WALL)

# --- lights ----------------------------------------------------------------------------------------------------------
lighting = group('Lighting')
light('Sun', lighting, (0, 30, 0), (-50.0, -35.0, 0.0), 'Directional', (1.0, 0.95, 0.88), 3.0, 200.0, True, 0.0)
for k in range(16):  # shadowed spots over the pillars, pointing down
    i, j = k % 4, k // 4
    x, z = (i - 1.5) * 12.0, (j - 1.5) * 12.0 - 6.0
    light(f'Spot {k + 1}', lighting, (x, 7.0, z), (-75.0, (k * 41) % 360, 0.0), 'Spot',
          (0.8 + 0.05 * (k % 4), 0.85, 1.0 - 0.04 * (k % 5)), 40.0, 14.0, True, 40.0)
for k in range(8):  # shadowed points between them
    a = k / 8.0 * 2.0 * math.pi
    light(f'Point Shadowed {k + 1}', lighting, (math.cos(a) * 16.0, 2.5, math.sin(a) * 16.0 - 6.0), (0, 0, 0),
          'Point', (1.0, 0.6, 0.35), 15.0, 9.0, True)
for k in range(160):  # unshadowed fill: clustered light-list load
    i, j = k % 16, k // 16
    x, z = (i - 7.5) * 4.0 + 1.0, (j - 4.5) * 6.0 - 7.0
    light(f'Point {k + 1}', lighting, (x, 1.2 + (k % 3) * 0.8, z), (0, 0, 0), 'Point',
          ((k * 0.13) % 1.0 * 0.6 + 0.4, (k * 0.29) % 1.0 * 0.6 + 0.4, (k * 0.47) % 1.0 * 0.6 + 0.4), 6.0, 6.0, False)
# Spinning casters under every shadowed light (Play only): the spot / point caches must redraw each frame.
spinners = group('Spinners')
for k in range(16):
    i, j = k % 4, k // 4
    e = base(f'Spinner Spot {k + 1}', spinners, ((i - 1.5) * 12.0 + 1.0, 2.5, (j - 1.5) * 12.0 - 6.0), (0, 0, 0),
             (1.2, 0.3, 1.2))
    e.update({'collider': {'friction': 0.8, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0], 'materials': ACCENT,
              'Transform Controller': {'Enabled': True, 'Rotation Deg/Sec': [0, 60 + k * 5, 30]}})
    ents['boxes'].append(e)
for k in range(8):
    a = k / 8.0 * 2.0 * math.pi
    e = base(f'Spinner Point {k + 1}', spinners, (math.cos(a) * 16.0 + 1.2, 1.2, math.sin(a) * 16.0 - 6.0), (0, 0, 0),
             (0.6, 1.2, 0.3))
    e.update({'collider': {'friction': 0.8, 'isTrigger': False}, 'color': [1.0, 1.0, 1.0], 'materials': ACCENT,
              'Transform Controller': {'Enabled': True, 'Rotation Deg/Sec': [0, 90, 0]}})
    ents['boxes'].append(e)

probe = base('Reflection Probe', lighting, (0, 4.0, -6.0))
probe['Reflection Probe'] = {'Importance': 0.5, 'Size': [70.0, 10.0, 70.0]}
ents['empties'].append(probe)

# --- decals ----------------------------------------------------------------------------------------------------------
decals = group('Decals')
for k in range(300):
    i, j = k % 30, k // 30
    decal(f'Wall Decal {k + 1}', decals, ((i - 14.5) * 1.5, 0.6 + j * 0.7, WALL_Z + 0.25), (0, 0, 0),
          (1.0, 1.0, 0.4), k)
for k in range(100):
    i, j = k % 10, k // 10
    decal(f'Floor Decal {k + 1}', decals, ((i - 4.5) * 6.0 + 3.0, 0.0, (j - 4.5) * 6.0 - 3.0), (-90.0, 0, 0),
          (2.0, 2.0, 0.4), 1000 + k)

# --- particles and glass ---------------------------------------------------------------------------------------------
fx = group('Effects')
for k in range(6):
    x = (k - 2.5) * 7.0
    e = base(f'Particles {k + 1}', fx, (x, 0.0, 6.0))
    e['Particle System'] = ({'Rate': 120.0, 'Lifetime': 1.2, 'Start Speed': 2.5, 'Spread': 15.0, 'Intensity': 4.0,
                             'Blend Mode': 1} if k % 2 == 0 else
                            {'Rate': 80.0, 'Lifetime': 2.0, 'Start Speed': 5.0, 'Spread': 30.0,
                             'Gravity Modifier': 1.0, 'Start Color': [0.6, 0.55, 0.5], 'End Color': [0.4, 0.38, 0.36],
                             'Start Alpha': 0.8, 'End Alpha': 0.0, 'Start Size': 0.3, 'End Size': 0.6,
                             'Blend Mode': 0})
    ents['empties'].append(e)
for k in range(8):
    x = (k - 3.5) * 4.0
    e = base(f'Glass {k + 1}', fx, (x, 1.2, 10.0), scale=(1.4, 1.4, 1.4))
    e.update({'path': f'primitive://sphere#{e["id"]}', 'materials': GLASS})
    ents['models'].append(e)

# --- camera ----------------------------------------------------------------------------------------------------------
cam = base('Bench Camera', -1, (0.0, 9.0, 24.0), (-14.0, 0.0, 0.0))
cam['Camera'] = {'Field of View': 60.0}
ents['empties'].append(cam)

scene = {
    'formatVersion': 4,
    '_comment': 'Generated by tools/gen_perf_stress_scene.py - do not hand edit. The --perf-bench stress scene.',
    'exposureEV': 0.0, 'tonemapOperator': 1, 'msaaSamples': 4, 'fxaa': False,
    'ssaoEnabled': True, 'ssaoIntensity': 1.0, 'ssaoRadius': 0.5, 'ssaoBias': 0.025,
    'bloomEnabled': True, 'bloomThreshold': 1.0, 'bloomKnee': 0.5, 'bloomIntensity': 0.25,
    'shadowsEnabled': True, 'shadowResolution': 4096, 'shadowCascades': 4, 'shadowDistance': 150.0,
    'maxSpotShadows': 16, 'maxPointShadows': 8,
    'skySource': 2, 'skyAmbientIntensity': 1.0,
    'sky': {'timeOfDay': True, 'timeOfDayHours': 16.5, 'animateInEditor': False, 'dayLengthMinutes': 0.0,
            'clouds': True, 'cloudCoverage': 0.5, 'cloudQuality': 1, 'cloudShadows': True,
            'cirrusCoverage': 0.3, 'aerialPerspectiveScale': 6.0},
    'volumetricEnabled': True, 'volDensity': 0.01, 'volDistance': 64.0, 'volQuality': 1, 'volAnisotropy': 0.6,
    'volBaseHeight': 0.0, 'volHeightFalloff': 0.0,
    'fogEnabled': False, 'motionBlur': False, 'depthOfField': False, 'chromaticAberration': False,
    'filmGrain': 0.0, 'vignetteIntensity': 0.0,
}
scene.update(ents)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(scene, f, indent=1, sort_keys=False)
    f.write('\n')
print(f'wrote {out}: {len(ents["boxes"])} boxes, {len(ents["empties"])} empties, {len(ents["models"])} models')
