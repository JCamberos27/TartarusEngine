# Generates the Sandbox scene's own assets: the materials (.mat: the material gallery on
# engine://StandardAdvanced.shader for clear coat / sheen / transmission / anisotropy / subsurface,
# the fountain water) and synthesized prop sounds (not used by the Sandbox itself).
# Everything is procedural (the standard library only), so it can be re-run.
# Usage: python tools/gen_sandbox_assets.py <repo root>
import json, math, os, random, struct, sys, wave

ROOT = sys.argv[1]
P = lambda *a: os.path.join(ROOT, 'project', *a)
for d in ('assets/Environments/Sandbox/Materials', 'assets/Audio'):
    os.makedirs(P(*d.split('/')), exist_ok=True)
random.seed(42)

# =============================================================================== materials
def mat(name, file, props, queue=None, comment=None, opacity=None, advanced=False):
    m = {'matVersion': 2, 'name': name}
    if comment: m['_comment'] = comment
    m['shader'] = 'engine://StandardAdvanced.shader' if advanced else 'engine://Standard.shader'
    if queue is not None: m['renderQueue'] = queue
    if opacity is not None: m['opacity'] = opacity
    m['properties'] = props
    with open(P('assets', 'Environments', 'Sandbox', 'Materials', file), 'w', newline='\n') as f:
        json.dump(m, f, indent=2)

def build_materials():
    # Material gallery: chrome and gold are plain Standard; the rest show the advanced lobes.
    mat('Gallery Chrome', 'gallery_chrome.mat', {'_BaseColor': [0.95, 0.95, 0.96], '_Metallic': 1.0, '_Roughness': 0.05})
    mat('Gallery Gold', 'gallery_gold.mat', {'_BaseColor': [1.0, 0.77, 0.34], '_Metallic': 1.0, '_Roughness': 0.22})
    mat('Gallery Brushed Steel', 'gallery_brushed.mat', {'_BaseColor': [0.8, 0.8, 0.82], '_Metallic': 1.0,
        '_Roughness': 0.35, '_Anisotropy': 0.85, '_AnisotropyRotation': 0.0}, comment='Anisotropic highlight.', advanced=True)
    mat('Gallery Car Paint', 'gallery_carpaint.mat', {'_BaseColor': [0.62, 0.03, 0.05], '_Metallic': 0.35,
        '_Roughness': 0.45, '_ClearCoat': 1.0, '_ClearCoatRoughness': 0.03}, comment='Clear coat over a metallic base.', advanced=True)
    mat('Gallery Velvet', 'gallery_velvet.mat', {'_BaseColor': [0.24, 0.05, 0.3], '_Metallic': 0.0, '_Roughness': 0.85,
        '_Sheen': [0.85, 0.55, 0.95], '_SheenRoughness': 0.3}, comment='Sheen lobe.', advanced=True)
    mat('Gallery Jade', 'gallery_jade.mat', {'_BaseColor': [0.3, 0.72, 0.45], '_Metallic': 0.0, '_Roughness': 0.25,
        '_SubsurfaceEnabled': True, '_SubsurfaceColor': [0.4, 0.95, 0.6], '_Thickness': 0.25}, comment='Subsurface.', advanced=True)
    mat('Gallery Glass', 'gallery_glass.mat', {'_BaseColor': [0.92, 0.97, 1.0], '_Metallic': 0.0, '_Roughness': 0.02,
        '_TransmissionStrength': 1.0, '_IOR': 1.5}, queue=2, comment='Transmission: refracts what is behind it.', advanced=True)
    mat('Gallery Neon', 'gallery_neon.mat', {'_BaseColor': [0.05, 0.3, 0.35], '_Metallic': 0.0, '_Roughness': 0.4,
        '_EmissiveColor': [0.2, 0.95, 1.0], '_EmissiveStrength': 6.0}, comment='Emissive: glows through Bloom.', advanced=True)
    mat('Fountain Water', 'water.mat', {'_BaseColor': [0.55, 0.78, 0.85], '_Metallic': 0.0, '_Roughness': 0.03,
        '_TransmissionStrength': 0.75, '_IOR': 1.33}, queue=2, opacity=0.7, comment='Refracts the basin floor.', advanced=True)

# =============================================================================== sounds
SR = 44100

def write_wav(name, samples, peak=0.9):
    m = max(1e-9, max(abs(s) for s in samples))
    k = peak / m
    with wave.open(P('assets', 'Audio', name), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(b''.join(struct.pack('<h', int(max(-1, min(1, s * k)) * 32767)) for s in samples))

def env_sine(t, f, decay, amp=1.0, phase=0.0):
    return amp * math.exp(-t / decay) * math.sin(2 * math.pi * f * t + phase)

def lowpass(xs, cutoff):
    a = 1 - math.exp(-2 * math.pi * cutoff / SR); y = 0.0; out = []
    for x in xs:
        y += a * (x - y); out.append(y)
    return out

def highpass(xs, cutoff):
    lp = lowpass(xs, cutoff)
    return [x - l for x, l in zip(xs, lp)]

def noise(n):
    return [random.uniform(-1, 1) for _ in range(n)]

def fade_in(xs, ms):
    n = int(SR * ms / 1000)
    return [x * min(1.0, i / max(n, 1)) for i, x in enumerate(xs)]

def build_sounds():
    # Wood knock (crates, bricks, dominoes).
    n = int(SR * 0.22)
    click = lowpass(noise(n), 4000)
    s = [env_sine(i / SR, 190, 0.03) + env_sine(i / SR, 430, 0.02, 0.6) + env_sine(i / SR, 910, 0.012, 0.35)
         + 0.7 * math.exp(-i / SR / 0.003) * click[i] for i in range(n)]
    write_wav('wood_knock.wav', fade_in(s, 0.2))
    # Rubber ball (playground balls, ball pit).
    n = int(SR * 0.3)
    s = [env_sine(i / SR, 420, 0.05) + env_sine(i / SR, 880, 0.025, 0.3) + 0.5 * math.exp(-i / SR / 0.004) * random.uniform(-1, 1)
         for i in range(n)]
    write_wav('rubber_ball.wav', fade_in(s, 0.3))
    # Heavy metal clank (the wrecking ball).
    n = int(SR * 1.6)
    hit = lowpass(noise(n), 1800)
    parts = [(96, 0.6, 1.0), (241, 0.45, 0.6), (395, 0.35, 0.45), (770, 0.25, 0.3), (1260, 0.15, 0.2)]
    s = [sum(env_sine(i / SR, f, dcy, a) for f, dcy, a in parts) + math.exp(-i / SR / 0.01) * hit[i] for i in range(n)]
    write_wav('metal_clank.wav', fade_in(s, 0.3))
    # Fountain: a seamless loop of splashing water (filtered noise + droplets).
    n = int(SR * 6.0)
    wash = highpass(lowpass(noise(n), 5200), 350)
    s = [0.35 * x * (0.85 + 0.15 * math.sin(2 * math.pi * 0.7 * i / SR)) for i, x in enumerate(wash)]
    for _ in range(140):
        at = random.randint(0, n - 1)
        f0 = random.uniform(900, 2600)
        for k in range(int(SR * 0.04)):
            t = k / SR
            idx = (at + k) % n
            s[idx] += 0.25 * math.exp(-t / 0.012) * math.sin(2 * math.pi * (f0 + 9000 * t) * t)
    # Crossfade the ends so it loops without a click.
    xf = int(SR * 0.25)
    for k in range(xf):
        a = k / xf
        s[k] = s[k] * a + s[n - xf + k] * (1 - a)
    write_wav('fountain_loop.wav', s[:n - xf], 0.7)

if __name__ == '__main__':
    steps = [('materials', build_materials), ('sounds', build_sounds)]
    only = set(sys.argv[2:])
    for name, fn in steps:
        if only and name not in only: continue
        print('building', name, flush=True)
        fn()
    print('done')
