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
    # SeedMesh Mossy Cedrus Forest shader graphs (sources are not in the pack; HDRP-style leftover props, see convert_seed)
    '212a9bade42d94343aad8c430fbd5665': 'seed',         # SeedMesh vegetation / ground / rock
    '418e3a61ec2e31d4a8f2efe6d641361c': 'seed',         # SeedMesh billboards
    '0ee7d7421b85b65448c87cdc62c1dc5c': 'seed',         # SeedMesh foliage cards (ferns, grass, leaves)
    '4328de05b5eecf74db8cc6d7b684a465': 'seed',         # SeedMesh bark / trunk blend
    'aa6663de77e9ce04d96f600b02434771': 'seed',         # SeedMesh branches
    '756bac9090102564582875f4c7e30202': 'hdrplit',      # HDRP/Lit Tessellation ("T_" variants): plain HDRP props, no displacement
    '6e4ae4064600d784cac1e41a9e6f2e59': 'lit',          # flat-colour display graph (Background_color, Ground)
    '41e99d5f2ee486e4f85e8cf7bf74a0bc': 'puddle',       # transparent puddle graph (only a water normal)
}
# SeedMesh wind (strength m, height m, flutter) by material-name pattern; first match wins. Billboards and mushrooms don't sway.
SEED_WIND = [(re.compile(r'billboard|mushroom', re.I), None),
             (re.compile(r'^Cedrus_leaves', re.I), (0.30, 14.0, 0.4)),
             (re.compile(r'.*'), (0.10, 0.8, 0.4))]
# Shader graphs that convert to Standard with a note until their own shader exists (by .shadergraph file name).
GRAPHS = {'Foliage': 'foliage', 'Water': 'water'}
# The engine's foliage shader params (_WindStrength = metres of sway at the top of _WindHeight; _WindFlutter = leaf flutter
# share), by material name; anything else gets the default. The pack's own graph wind values are not stored in the .mat.
FOLIAGE_WIND = {
    'Tree': (0.30, 10.0, 0.35), 'Plant_Treebush': (0.10, 3.0, 0.5), 'Plant_Hedgebush': (0.06, 2.0, 0.5),
    'Plant_BushGrass': (0.10, 1.0, 0.4), 'Plant_Grass': (0.12, 0.6, 0.3), 'Plant_Flowers': (0.10, 0.8, 0.3),
    'ClimberPlant': (0.05, 3.0, 0.5),
}


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
            self.report['missing textures (dangling in the source pack: asset deleted, nothing to convert)'].append(f'{guid} (used by {getattr(self, "cur", "?")})')
            return ''
        key = (src, json.dumps(expr), suffix)
        if key in self.cooked:
            return self.cooked[key]
        ext = os.path.splitext(src)[1].lower()
        w, h = image_size(src)
        plain = expr in ('rgba', 'rgb') and not suffix
        if plain and ext in ('.png', '.tga', '.jpg') and max(w, h) <= MAX_TEX:
            dst = self.out_path(src)
            if expr == 'rgb' and has_alpha_data(src):
                # The engine's mip chain weights colour by alpha, so an opaque material whose albedo carries alpha (an
                # atlas whose alpha is 0 where the artist painted nothing, smoothness leftovers) goes black in the distance
                # (the Office pack's wall flags). Opaque use gets an RGB-only copy next to the original.
                dst = self.out_path(src, '_rgb' + ext)
                strip_alpha(src, dst)
            else:
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
            self.report['missing materials (dangling in the source pack: asset deleted)'].append(f'{guid} (used by {getattr(self, "cur", "?")})')
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
        prev, self.cur = getattr(self, 'cur', '?'), 'material ' + name
        props, extra = self.convert_material(name, kind, tex, fl, co, keywords)
        self.cur = prev
        dst = os.path.join(os.path.dirname(self.out_path(src, '.mat')), 'M_' + tidy(name) + '.mat')
        data = {'factorsScaleMaps': True, 'matVersion': 2, 'name': 'M_' + tidy(name), 'properties': props,
                'shader': 'engine://Standard.shader'}
        data.update(extra)
        write_json(dst, data)
        write_meta(dst, os.path.dirname(rel_project(self.repo, dst)), 'material')
        self.mats[guid] = rel_project(self.repo, dst)
        self.counts['materials'] += 1
        if kind == 'foliage':
            self.report['Foliage (engine foliage wind + leaf translucency; params from FOLIAGE_WIND)'].append(name)
        elif kind in ('seed', 'hdrplit', 'puddle'):
            self.counts['seedmesh materials'] += 1
        elif kind not in ('lit', 'unlit'):
            self.report[f'materials approximated on Standard ({kind})'].append(name)
        return self.mats[guid]

    def convert_seed(self, name, tex, fl, co, keywords, hdrp=False):
        """SeedMesh graphs: the exposed props are _MainTex (albedo + alpha), Normal_vegetation, mask_vegetation (HDRP mask:
        R metallic, G AO, A smoothness; no metal anywhere in the pack) and _Color; _BaseColorMap / _MaskMap / _NormalMap are
        stale HDRP leftovers (often a different texture). _ASSET_TYPE 0 common, 1 foliage, 2 moss variant. _Covering_* /
        _Snow_*_sm layers become the Standard shaders' _Cover* world-up blend (seed_cover.py)."""
        def first(*keys):
            for k in keys:
                if k in tex:
                    return tex[k]['m_Texture']['guid'], tex[k]
            return None, None
        c = co.get('_Color') or co.get('_BaseColor') or {}
        p = {'_AlbedoMap': '', '_AOMap': '', '_BaseColor': [round(c.get(k, 1.0), 4) for k in 'rgb'], '_DoubleSided': False,
             '_EmissiveColor': [0.0, 0.0, 0.0], '_EmissiveMap': '', '_EmissiveStrength': 1.0, '_Metallic': 0.0,
             '_MetallicMap': '', '_MetallicRoughnessMap': '', '_NormalMap': '', '_Roughness': 0.9, '_RoughnessMap': '',
             '_Triplanar': False, '_TriplanarScale': 1.0, '_ReflectionProbes': True}
        cutout = fl.get('_AlphaCutoffEnable', 0) == 1 or '_ALPHATEST_ON' in keywords
        if hdrp:   # real HDRP/Lit props
            g, t = first('_BaseColorMap', '_MainTex')
        else:
            g, t = first('_MainTex', '_BaseColorMap', '_BaseColorMap1', 'base_color_moss')
        if g:
            p['_AlbedoMap'] = self.texture(g, 'rgba' if cutout else 'rgb', 'albedo')
            sc, off = t.get('m_Scale') or {}, t.get('m_Offset') or {}
            if (sc.get('x', 1), sc.get('y', 1)) != (1, 1):
                p['_UVTiling'] = [sc.get('x', 1), sc.get('y', 1)]
            if (off.get('x', 0), off.get('y', 0)) != (0, 0):
                p['_UVOffset'] = [off.get('x', 0), off.get('y', 0)]
        g, _ = first('_MaskMap', 'mask_vegetation') if hdrp else first('mask_vegetation', '_MaskMap', '_MaskMap1', 'mask_moss')
        if g:
            lo, hi = fl.get('_SmoothnessRemapMin', 0.0), fl.get('_SmoothnessRemapMax', 1.0)
            if hi <= lo:
                lo, hi = 0.0, 1.0
            k = hi - lo   # roughness = 1 - (lo + A*(hi-lo))
            p['_RoughnessMap'] = self.texture(g, ['ch', 3, -k, 1.0 - lo], 'data', f'_Roughness{round(k * 100)}_{round(lo * 100)}')
            p['_Roughness'] = 1.0
            p['_AOMap'] = self.texture(g, ['ao', 1, 1.0], 'data', '_AO100')
        g, _ = first('_NormalMap', 'Normal_vegetation') if hdrp else first('Normal_vegetation', '_NormalMap', '_NormalMap1', 'normal_moss')
        if g:
            p['_NormalMap'] = self.texture(g, 'rgb', 'normal')
            ns = fl.get('_NormalScale', 1.0) if hdrp else fl.get('Normal_intensity_vegetation', 1.0)
            if ns != 1.0:
                p['_NormalStrength'] = ns
        extra = {}
        if fl.get('_CullMode', 2) == 0 or fl.get('_DoubleSidedEnable', 0) == 1:
            p['_DoubleSided'] = True
        if cutout:
            extra['renderQueue'] = 1
            extra['alphaCutoff'] = round(fl.get('_AlphaCutoff', 0.5), 4)
        foliage = fl.get('_ASSET_TYPE', 0) == 1 or fl.get('_MaterialTypeMask', 2) == 32 or 'billboard' in name.lower()
        if foliage:
            p['_DoubleSided'] = True
            p['_Translucency'] = 0.6
            wind = next(w for rx, w in SEED_WIND if rx.search(name))
            if wind:
                p.update({'_Foliage': True, '_WindStrength': wind[0], '_WindHeight': wind[1], '_WindFlutter': wind[2]})
        if any(k.startswith(('_Covering', '_Snow', '_COVERING')) for k in list(tex) + list(fl)):
            import seed_cover
            if seed_cover.add_cover(self, name, tex, fl, p, foliage):
                self.report['SeedMesh _Covering layer (moss, world-up blend: Standard.shader _Cover*)'].append(name)
            else:
                self.report['SeedMesh _Covering / _Snow layers (unused or foliage: base layer drawn)'].append(name)
        return p, extra

    def convert_material(self, name, kind, tex, fl, co, keywords):
        if kind in ('seed', 'hdrplit', 'puddle'):
            p, extra = self.convert_seed(name, tex, fl, co, keywords, hdrp=kind == 'hdrplit')
            if kind == 'puddle':
                p.update({'_BaseColor': [0.12, 0.16, 0.16], '_Roughness': 0.04, '_RoughnessMap': '', '_AOMap': '', '_ReceiveDecals': False})
                extra.update({'renderQueue': 2, 'opacity': 0.55})
            return p, extra
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
             '_Triplanar': False, '_TriplanarScale': 1.0, '_ReflectionProbes': True}
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
            if not p['_MetallicMap']:   # the mask texture was deleted from the pack: the slider alone would make it chrome
                p['_Metallic'], p['_Roughness'] = 0.0, round(1.0 - smooth, 4) if smooth < 1.0 else 0.85
                p['_RoughnessMap'] = ''
        elif kind != 'unlit':
            p['_Metallic'] = round(fl.get('_Metallic', 0.0), 4)
            p['_Roughness'] = round(1.0 - smooth, 4)
        if kind == 'foliage':
            # plants are never metal; a foliage MaskMap's R channel is not metallic in any case
            p['_MetallicMap'], p['_Metallic'] = '', 0.0
            p['_RoughnessMap'], p['_Roughness'] = '', 0.85   # Smoothness 1 on leaves rendered black mirrors
        if kind == 'foliage':
            strength, height, flutter = FOLIAGE_WIND.get(name, (0.12, 4.0, 0.3))
            p.update({'_Foliage': True, '_WindStrength': strength, '_WindHeight': height, '_WindFlutter': flutter,
                      '_Translucency': 0.6})
        if kind == 'water':
            p['_Roughness'] = min(p['_Roughness'], 0.05)
        g, _ = guid_of('_OcclusionMap')
        if g and kind != 'unlit':
            strength = fl.get('_OcclusionStrength', 1.0)
            p['_AOMap'] = self.texture(g, ['ao', 1, strength], 'data', f'_AO{round(strength * 100)}')
        g, t = guid_of('_BumpMap', '_NormalMap')
        if g:
            p['_NormalMap'] = self.texture(g, 'rgb', 'normal')
            if fl.get('_BumpScale', 1.0) != 1.0:
                p['_NormalStrength'] = fl['_BumpScale']
            if kind == 'water':   # the pack's 0.03 flattens the ripples away
                p['_NormalStrength'] = max(p.get('_NormalStrength', 1.0), 0.3)
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
        self.cur = os.path.basename(src)
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


def has_alpha_data(path):
    """True when the image has an alpha channel that is not fully opaque."""
    try:
        from PIL import Image
        with Image.open(path) as im:
            if im.mode not in ('RGBA', 'LA', 'PA') and not (im.mode == 'P' and 'transparency' in im.info):
                return False
            return im.convert('RGBA').getchannel('A').getextrema()[0] < 255
    except Exception:
        return False


def strip_alpha(src, dst):
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    from PIL import Image
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with Image.open(src) as im:
        im.convert('RGB').save(dst)


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
