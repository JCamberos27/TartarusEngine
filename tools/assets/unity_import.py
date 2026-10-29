# Imports a Unity Asset Store pack (URP) into project/assets/Environments/<Name>, laid out like the pack itself:
#   - models    -> the pack's .fbx files, each .meta with a materialRemap onto the converted materials
#   - materials -> one engine .mat per Unity .mat (URP Lit / Unlit; others are reported), its maps cooked to PNG where
#                  Unity packs channels (MetallicGloss / Occlusion -> metallic, roughness and AO maps), the rest copied
#   - prefabs   -> every .prefab as an engine .prefab: hierarchy, renderers, box / sphere / capsule / mesh colliders and
#                  LOD groups, nested prefabs expanded
#   - scenes    -> each demo .unity as project/scenes/<Name>_<Scene>.json
#   - a report  -> project/assets/Environments/<Name>/unity_import_report.md: what came across and what didn't
# Textures are capped at 2048 (4K sources are halved); the cook runs in Blender (unity_tex_cook.py), which reads TGA,
# TIF, PSD and EXR. Placement comes from the engine's own read of each FBX (--model-nodes), so the exe must be built.
# The payloads (fbx, png, tga) are git-ignored; afterwards run asset_manifest.py build + export.
#
#   python tools/assets/unity_import.py <repo> <Unity pack dir> [--name <Name>] [--exe <exe>] [--blender <exe>]
#                                       [--assets <Unity Assets dir>] [--skip-cook]
import argparse
import collections
import json
import math
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import unity_yaml as uy  # noqa: E402
from import_model_library import write_json, write_meta  # noqa: E402

BLENDER = r'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe'
MAX_TEX = 2048

# Unity shader guid -> what it converts as. fileID 106 (guid 0000000000000000f000000000000000) is Skybox/Procedural.
SHADERS = {
    '933532a4fcc9baf4fa0491de14d08ed7': 'lit',          # Universal Render Pipeline/Lit
    '8d2bb70cbf9db8d4da26e15b26e74248': 'lit',          # Universal Render Pipeline/Simple Lit
    '650dd9526735d5b46b79224bc6e94025': 'unlit',        # Universal Render Pipeline/Unlit
    'b7839dad95683814aa64166edc107ae2': 'particle',     # Universal Render Pipeline/Particles/Unlit
    '0406db5a14f94604a8c57ccfbc9f3b46': 'particle',     # Universal Render Pipeline/Particles/Simple Lit
    '0000000000000000f000000000000000': 'skybox',
}
# Shader graphs that convert to Standard with a note until their own shader exists (by .shadergraph file name).
GRAPHS = {'Foliage': 'foliage', 'Water': 'water'}


def rel_project(repo, p):
    return os.path.relpath(p, os.path.join(repo, 'project')).replace(os.sep, '/')


def tidy(name):
    return re.sub(r'[^A-Za-z0-9_\-]+', '_', name).strip('_')


class Pack:
    def __init__(self, repo, src, name, assets):
        self.repo, self.src, self.name = repo, os.path.abspath(src), name
        self.assets = os.path.abspath(assets or os.path.dirname(self.src))
        self.dest = os.path.join(repo, 'project', 'assets', 'Environments', name)
        self.report = collections.defaultdict(list)
        self.counts = collections.Counter()
        self.guids = {}       # unity guid -> source path (whole Assets folder: packs reference shared files)
        self.metas = {}       # unity guid -> parsed .meta
        self.mats = {}        # unity material guid -> engine .mat project path (or None: not convertible)
        self.models = {}      # unity model guid -> engine fbx project path
        self.cook = []        # Blender jobs
        self.cooked = {}      # (src, expr, max) -> engine texture project path
        self.textures = {}    # engine texture project path -> role ('albedo', 'normal', 'data')

    # --- index ---------------------------------------------------------------------------------------------------
    def index(self):
        rx = re.compile(r'^guid: ([0-9a-f]{32})', re.M)
        for folder, _, files in os.walk(self.assets):
            for f in files:
                if f.endswith('.meta'):
                    p = os.path.join(folder, f)
                    with open(p, encoding='utf-8', errors='replace') as h:
                        m = rx.search(h.read(4096))
                    if m:
                        self.guids[m.group(1)] = p[:-5]

    def meta(self, guid):
        if guid not in self.metas:
            self.metas[guid] = uy.load(uy.read(self.guids[guid] + '.meta'))
        return self.metas[guid]

    def out_path(self, src, ext=None):
        """Where a pack file lands: the same path under Environments/<Name> (files from outside the pack go to
        Environments/<Name>/_Shared/...)."""
        r = os.path.relpath(src, self.src)
        if r.startswith('..'):
            r = os.path.join('_Shared', os.path.relpath(src, self.assets))
        if ext:
            r = os.path.splitext(r)[0] + ext
        return os.path.join(self.dest, r)

    # --- textures ------------------------------------------------------------------------------------------------
    def texture(self, guid, expr, role, suffix=''):
        """Engine path of texture <guid> through <expr> (unity_tex_cook.py), copying it as-is where that's enough."""
        src = self.guids.get(guid)
        if not src:
            self.report['missing textures'].append(guid)
            return ''
        key = (src, json.dumps(expr), suffix)
        if key in self.cooked:
            return self.cooked[key]
        ext = os.path.splitext(src)[1].lower()
        w, h = image_size(src)
        plain = expr in ('rgba', 'rgb') and not suffix
        if plain and ext in ('.png', '.tga', '.jpg') and max(w, h) <= MAX_TEX:
            dst = self.out_path(src)
            copy_if_changed(src, dst)
        else:
            dst = self.out_path(src, (suffix or '') + '.png')
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
                self.cook.append({'src': src, 'max': MAX_TEX, 'outs': [
                    {'path': dst, 'expr': expr, 'srgb': role == 'albedo', 'alpha': expr == 'rgba'}]})
            if max(w, h) > MAX_TEX:
                self.counts['textures halved'] += 1
        p = rel_project(self.repo, dst)
        self.cooked[key] = p
        self.textures[p] = role
        return p

    def write_texture_metas(self):
        for p, role in self.textures.items():
            full = os.path.join(self.repo, 'project', *p.split('/'))
            write_meta(full, os.path.dirname(p), 'texture', {'importer': {
                'anisoLevel': 8, 'compression': 1, 'filterMode': 2, 'generateMipmaps': True,
                'isSRGB': role == 'albedo', 'maxTextureSize': MAX_TEX, 'textureType': 1 if role == 'normal' else 0,
                'wrapMode': 0}})
            self.counts['textures'] += 1

    # --- materials -----------------------------------------------------------------------------------------------
    def material(self, guid):
        if guid in self.mats:
            return self.mats[guid]
        src = self.guids.get(guid)
        self.mats[guid] = None
        if not src or not src.endswith('.mat'):
            self.report['missing materials'].append(guid)
            return None
        doc = uy.load(uy.read(src).split('\n', 3)[3])['Material']
        name = doc.get('m_Name') or os.path.splitext(os.path.basename(src))[0]
        sh = doc.get('m_Shader') or {}
        kind = SHADERS.get(sh.get('guid'))
        if not kind and sh.get('guid') in self.guids:
            graph = os.path.splitext(os.path.basename(self.guids[sh['guid']]))[0]
            kind = GRAPHS.get(graph, 'graph:' + graph)
        if kind in ('skybox', 'particle') or not kind:
            self.report[f'materials not converted ({kind or "unknown shader " + str(sh.get("guid"))})'].append(name)
            return None
        tex = {}
        for e in (doc.get('m_SavedProperties') or {}).get('m_TexEnvs') or []:
            for k, v in e.items():
                if v and (v.get('m_Texture') or {}).get('guid'):
                    tex[k] = v
        fl, co = {}, {}
        for e in (doc.get('m_SavedProperties') or {}).get('m_Floats') or []:
            fl.update(e)
        for e in (doc.get('m_SavedProperties') or {}).get('m_Colors') or []:
            co.update(e)
        keywords = set(doc.get('m_ValidKeywords') or []) | set((doc.get('m_ShaderKeywords') or '').split())
        props, extra = self.convert_material(name, kind, tex, fl, co, keywords)
        dst = self.out_path(src, '.mat')
        dst = os.path.join(os.path.dirname(dst), 'M_' + tidy(name) + '.mat')
        data = {'factorsScaleMaps': True, 'matVersion': 2, 'name': 'M_' + tidy(name), 'properties': props,
                'shader': 'engine://Standard.shader'}
        data.update(extra)
        write_json(dst, data)
        write_meta(dst, os.path.dirname(rel_project(self.repo, dst)), 'material')
        self.mats[guid] = rel_project(self.repo, dst)
        self.counts['materials'] += 1
        if kind not in ('lit', 'unlit'):
            self.report[f'materials approximated on Standard ({kind})'].append(name)
        return self.mats[guid]

    def convert_material(self, name, kind, tex, fl, co, keywords):
        def colour(k, d=(1.0, 1.0, 1.0, 1.0)):
            c = co.get(k)
            return [c['r'], c['g'], c['b'], c.get('a', 1.0)] if c else list(d)

        def guid_of(*keys):
            for k in keys:
                if k in tex:
                    return tex[k]['m_Texture']['guid'], tex[k]
            return None, None

        p = {'_AlbedoMap': '', '_AOMap': '', '_BaseColor': [1.0, 1.0, 1.0], '_DoubleSided': False,
             '_EmissiveColor': [0.0, 0.0, 0.0], '_EmissiveMap': '', '_EmissiveStrength': 1.0, '_Metallic': 0.0,
             '_MetallicMap': '', '_MetallicRoughnessMap': '', '_NormalMap': '', '_Roughness': 0.5, '_RoughnessMap': '',
             '_Triplanar': False, '_TriplanarScale': 1.0}
        extra = {}
        base = colour('_BaseColor', colour('_Color'))
        p['_BaseColor'] = [round(v, 4) for v in base[:3]]
        g, t = guid_of('_BaseMap', '_MainTex', '_BaseColorMap', '_Albedo', '_AlbedoMap')
        cutout = fl.get('_AlphaClip', 0) == 1 or '_ALPHATEST_ON' in keywords or kind == 'foliage'
        transparent = fl.get('_Surface', 0) == 1 or kind == 'water'
        if g:
            p['_AlbedoMap'] = self.texture(g, 'rgba' if (cutout or transparent) else 'rgb', 'albedo')
            sc, off = t.get('m_Scale') or {}, t.get('m_Offset') or {}
            if (sc.get('x', 1), sc.get('y', 1)) != (1, 1):
                p['_UVTiling'] = [sc.get('x', 1), sc.get('y', 1)]
            if (off.get('x', 0), off.get('y', 0)) != (0, 0):
                p['_UVOffset'] = [off.get('x', 0), off.get('y', 0)]
        if kind == 'unlit':
            # No unlit shader: emissive-only Standard (black albedo, the map as emission), which reads the same.
            p['_EmissiveMap'], p['_AlbedoMap'] = p['_AlbedoMap'], ''
            p['_EmissiveColor'], p['_BaseColor'] = p['_BaseColor'], [0.0, 0.0, 0.0]
            p['_Roughness'], p['_SpecularHighlights'], p['_GlossyReflections'] = 1.0, False, False
        smooth = fl.get('_Smoothness', fl.get('_Glossiness', 0.5))
        g, _ = guid_of('_MetallicGlossMap', '_MaskMap')
        if g and kind != 'unlit':
            p['_MetallicMap'] = self.texture(g, ['ch', 0, 1.0, 0.0], 'data', '_Metallic')
            p['_Metallic'] = 1.0
            if fl.get('_SmoothnessTextureChannel', 0) == 1 and p['_AlbedoMap']:
                ga, _ = guid_of('_BaseMap', '_MainTex')
                p['_RoughnessMap'] = self.texture(ga, ['ch', 3, -smooth, 1.0], 'data', f'_Roughness{round(smooth * 100)}')
            else:
                p['_RoughnessMap'] = self.texture(g, ['ch', 3, -smooth, 1.0], 'data', f'_Roughness{round(smooth * 100)}')
            p['_Roughness'] = 1.0
        elif kind != 'unlit':
            p['_Metallic'] = round(fl.get('_Metallic', 0.0), 4)
            p['_Roughness'] = round(1.0 - smooth, 4)
        g, _ = guid_of('_OcclusionMap')
        if g and kind != 'unlit':
            strength = fl.get('_OcclusionStrength', 1.0)
            p['_AOMap'] = self.texture(g, ['ao', 1, strength], 'data', f'_AO{round(strength * 100)}')
        g, t = guid_of('_BumpMap', '_NormalMap')
        if g:
            p['_NormalMap'] = self.texture(g, 'rgb', 'normal')
            if fl.get('_BumpScale', 1.0) != 1.0:
                p['_NormalStrength'] = fl['_BumpScale']
        g, _ = guid_of('_ParallaxMap', '_HeightMap')
        if g:
            p['_HeightMap'] = self.texture(g, ['ch', 1, 1.0, 0.0], 'data', '_Height')
            p['_ParallaxScale'] = round(min(fl.get('_Parallax', 0.02), 0.1), 4)
        em = colour('_EmissionColor', (0, 0, 0, 1))
        g, _ = guid_of('_EmissionMap')
        if kind != 'unlit' and ('_EMISSION' in keywords or g) and max(em[:3]) > 0:
            peak = max(em[:3])
            p['_EmissiveColor'] = [round(v / peak, 4) for v in em[:3]] if peak > 1 else [round(v, 4) for v in em[:3]]
            p['_EmissiveStrength'] = round(max(peak, 1.0), 4)
            if g:
                p['_EmissiveMap'] = self.texture(g, 'rgb', 'albedo')
        g, t = guid_of('_DetailAlbedoMap')
        if g:
            p['_DetailAlbedoMap'] = self.texture(g, 'rgb', 'albedo')
            sc = t.get('m_Scale') or {}
            p['_DetailTiling'] = [sc.get('x', 1), sc.get('y', 1)]
        g, _ = guid_of('_DetailNormalMap')
        if g:
            p['_DetailNormalMap'] = self.texture(g, 'rgb', 'normal')
        if fl.get('_Cull', 2) == 0 or kind == 'foliage':
            p['_DoubleSided'] = True
        if transparent:
            extra['renderQueue'] = 2
            extra['opacity'] = round(base[3], 4)
            p['_ReceiveDecals'] = False
        elif cutout:
            extra['renderQueue'] = 1
            extra['alphaCutoff'] = round(fl.get('_Cutoff', fl.get('_AlphaClipThreshold', 0.5)), 4)
        if fl.get('_WorkflowMode', 1) == 0:
            self.report['specular-workflow materials (converted as metallic)'].append(name)
        return p, extra

    # --- models --------------------------------------------------------------------------------------------------
    def model(self, guid):
        """Copy the FBX and write its .meta (materialRemap from the Unity importer's externalObjects)."""
        if guid in self.models:
            return self.models[guid]
        src = self.guids.get(guid)
        if not src or not src.lower().endswith('.fbx'):
            self.models[guid] = None
            self.report['missing / non-FBX models'].append(src or guid)
            return None
        dst = self.out_path(src)
        copy_if_changed(src, dst)
        imp = self.meta(guid).get('ModelImporter') or {}
        meshes = imp.get('meshes') or {}
        remap = {}
        # collision-only FBX: never drawn, their leftover material slots point at deleted materials
        collision = 'colliders' in os.path.relpath(src, self.src).lower().split(os.sep)[:-1]
        for e in [] if collision else imp.get('externalObjects') or []:
            first, second = e.get('first') or {}, e.get('second') or {}
            if first.get('type') == 'UnityEngine:Material' and second.get('guid'):
                m = self.material(second['guid'])
                if m:
                    remap[first['name']] = m
        scale = meshes.get('globalScale', 1) or 1
        extra = {'importer': {'excludeNodes': [], 'globalScale': float(scale), 'importAnimations': False,
                              'importNormals': True, 'importSkeleton': False, 'materialImportMode': 1,
                              'mergeByMaterial': True, 'optimizeGraph': True}, 'materialRemap': remap}
        if meshes.get('useFileScale', 1) == 0:
            self.report['models with Use File Scale off (check their size)'].append(os.path.basename(src))
        write_meta(dst, os.path.dirname(rel_project(self.repo, dst)), 'model', extra)
        self.models[guid] = rel_project(self.repo, dst)
        self.counts['models'] += 1
        return self.models[guid]


def copy_if_changed(src, dst):
    if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)


def image_size(path):
    """(w, h) from the file header for PNG / TGA / JPG / TIF / EXR / PSD; (0, 0) if unknown."""
    import struct
    try:
        with open(path, 'rb') as f:
            b = f.read(65536)
    except OSError:
        return 0, 0
    ext = os.path.splitext(path)[1].lower()
    try:
        if b[:8] == b'\x89PNG\r\n\x1a\n':
            return struct.unpack('>II', b[16:24])
        if ext == '.tga':
            return struct.unpack('<HH', b[12:16])
        if b[:4] == b'8BPS':
            h, w = struct.unpack('>II', b[14:22])
            return w, h
        if b[:2] in (b'II', b'MM'):
            e = '<' if b[:2] == b'II' else '>'
            off = struct.unpack(e + 'I', b[4:8])[0]
            n = struct.unpack(e + 'H', b[off:off + 2])[0]
            w = h = 0
            for i in range(n):
                tag, typ, _, val = struct.unpack(e + 'HHII', b[off + 2 + i * 12:off + 14 + i * 12])
                if typ == 3:
                    val = struct.unpack(e + 'H', b[off + 10 + i * 12:off + 12 + i * 12])[0]
                if tag == 256:
                    w = val
                elif tag == 257:
                    h = val
            return w, h
        if b[:2] == b'\xff\xd8':
            i = 2
            while i < len(b):
                if b[i] != 0xFF:
                    i += 1
                    continue
                marker = b[i + 1]
                if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
                    h, w = struct.unpack('>HH', b[i + 5:i + 9])
                    return w, h
                i += 2 + struct.unpack('>H', b[i + 2:i + 4])[0]
        if b[:4] == b'\x76\x2f\x31\x01':  # EXR: dataWindow attribute
            i = b.find(b'dataWindow\x00box2i\x00')
            if i >= 0:
                x0, y0, x1, y1 = struct.unpack('<iiii', b[i + 21:i + 37])
                return x1 - x0 + 1, y1 - y0 + 1
    except struct.error:
        pass
    return 0, 0


def run_cook(pack, blender, scratch):
    if not pack.cook:
        return
    jobs = os.path.join(scratch, f'texcook_{pack.name}.json')
    # merge jobs that read the same source: one load per file
    merged = collections.OrderedDict()
    for j in pack.cook:
        merged.setdefault(j['src'], {'src': j['src'], 'max': j['max'], 'outs': []})['outs'].extend(j['outs'])
    os.makedirs(scratch, exist_ok=True)
    # Blender's own parallelism is weak for this: split into 6 processes.
    items = list(merged.values())
    procs = []
    for n in range(6):
        part = items[n::6]
        if not part:
            continue
        path = jobs.replace('.json', f'_{n}.json')
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(part, f)
        procs.append(subprocess.Popen([blender, '-b', '--factory-startup', '--python',
                                       os.path.join(os.path.dirname(__file__), 'unity_tex_cook.py'), '--', path],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace'))
    for p in procs:
        out = p.communicate()[0]
        for line in out.splitlines():
            if line.startswith('[TexCook] FAILED'):
                pack.report['textures that failed to cook'].append(line[17:])
    pack.counts['textures cooked'] += sum(len(j['outs']) for j in items)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('pack')
    ap.add_argument('--name')
    ap.add_argument('--assets', help='the Unity project Assets folder (default: the pack folder\'s parent)')
    ap.add_argument('--exe')
    ap.add_argument('--blender', default=BLENDER)
    ap.add_argument('--skip-cook', action='store_true')
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    name = args.name or tidy(os.path.basename(os.path.normpath(args.pack)))
    exe = os.path.abspath(args.exe) if args.exe else os.path.join(repo, 'b', 'Release', 'TartarusEngine.exe')
    assets = args.assets
    if not assets:  # SeedMesh/<Pack>: the Assets folder is the first parent holding a sibling "Packages"-less tree
        assets = os.path.dirname(os.path.abspath(args.pack))
        while os.path.basename(assets) != 'Assets' and os.path.dirname(assets) != assets:
            assets = os.path.dirname(assets)
    pack = Pack(repo, args.pack, name, assets)
    pack.index()
    import unity_prefabs  # noqa: E402  (placement; needs the exe)
    unity_prefabs.convert(pack, exe)
    if not args.skip_cook:
        run_cook(pack, args.blender, os.path.join(repo, 'b', 'unity_import'))
    pack.write_texture_metas()
    unity_prefabs.write_report(pack)


if __name__ == '__main__':
    main()
