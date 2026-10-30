"""SeedMesh cover layer (moss / snow over up-facing surfaces) for the Standard shaders' _Cover* properties.

The pack's materials carry the layer as _Snow_Base_Color_sm / _Snow_Normal_sm / _Snow_Mask_sm / _Covering_Height (all
87 layered materials point at the same SeedMesh_Moss_surface_* set: it is moss, not snow) and blend it with
_Blend_Amount (edge sharpness; 0 = layer unused), _Covering_Height_Intensity + _Use_height_to_blend (height breakup) and
_Overall_Covering (strength). The sources are shader graphs, so the world-up blend itself is the engine's own
(Standard.shader _Cover*); these are the parameters read off the .mat.

usage: seed_cover.py <repo> <Unity SeedMesh pack folder> [--dry]   (re-points the already converted M_*.mat files)
"""
import json
import os
import sys

COVER_TEX = ('_Snow_Base_Color_sm', '_Snow_Normal_sm', '_Snow_Mask_sm', '_Covering_Height')


def cover_params(name, tex, fl, foliage):
    """The _CoverStrength/Sharpness/Height/... values for a material, or None when it has no cover layer."""
    if foliage or not all(k in tex for k in COVER_TEX):
        return None
    blend = fl.get('_Blend_Amount', 0)
    if blend <= 0 or fl.get('_Covering_sm', 1) != 1:
        return None
    use_h = fl.get('_Use_height_to_blend', 0) == 1
    return {'_CoverStrength': round(min(1.0, fl.get('_Overall_Covering', 1.0)), 3),
            '_CoverSharpness': round(min(max(blend, 0.5), 30.0), 3),
            '_CoverHeight': round(fl.get('_Covering_Height_Intensity', 0.0), 3) if use_h else 0.0,
            '_CoverScale': 0.5, '_CoverThreshold': 0.6}


def pack_mask(pack, mask_guid, height_guid):
    """Engine path of the cover mask: R height, G occlusion, B smoothness (opaque RGB so mips never weight by alpha)."""
    from PIL import Image
    ms, hs = pack.guids.get(mask_guid), pack.guids.get(height_guid)
    if not ms or not hs:
        return ''
    dst = pack.out_path(ms, '_CoverMask.png')
    if not os.path.exists(dst) or os.path.getmtime(dst) < max(os.path.getmtime(ms), os.path.getmtime(hs)):
        m = Image.open(ms).convert('RGBA')
        size = (min(m.width, 2048), min(m.height, 2048))
        m = m.resize(size, Image.LANCZOS)
        h = Image.open(hs).convert('RGB').resize(size, Image.LANCZOS)
        r, g, b, a = m.split()
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        Image.merge('RGB', (h.split()[0], g, a)).save(dst)
    p = os.path.relpath(dst, os.path.join(pack.repo, 'project')).replace(os.sep, '/')
    pack.textures[p] = 'data'
    return p


def add_cover(pack, name, tex, fl, p, foliage):
    cp = cover_params(name, tex, fl, foliage)
    if not cp:
        return False
    g = lambda k: tex[k]['m_Texture']['guid']
    p['_CoverAlbedoMap'] = pack.texture(g('_Snow_Base_Color_sm'), 'rgb', 'albedo')
    p['_CoverNormalMap'] = pack.texture(g('_Snow_Normal_sm'), 'rgb', 'normal')
    p['_CoverMaskMap'] = pack_mask(pack, g('_Snow_Mask_sm'), g('_Covering_Height'))
    if not (p['_CoverAlbedoMap'] and p['_CoverNormalMap'] and p['_CoverMaskMap']):
        for k in ('_CoverAlbedoMap', '_CoverNormalMap', '_CoverMaskMap'):
            p.pop(k, None)
        return False
    p.update(cp)
    pack.counts['materials with a cover layer'] += 1
    return True


def main():
    repo, src = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    dry = '--dry' in sys.argv
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import unity_import as ui
    from import_model_library import write_json
    assets = os.path.dirname(src)
    while os.path.basename(assets) != 'Assets' and os.path.dirname(assets) != assets:
        assets = os.path.dirname(assets)
    pack = ui.Pack(repo, src, 'SeedMesh_' + ui.tidy(os.path.basename(os.path.normpath(src))), assets)
    pack.index()
    n = 0
    for guid, path in list(pack.guids.items()):
        if not path.endswith('.mat') or not os.path.abspath(path).startswith(src):
            continue
        doc = ui.uy.load(ui.uy.read(path).split('\n', 3)[3])['Material']
        if ui.SHADERS.get((doc.get('m_Shader') or {}).get('guid')) != 'seed':
            continue
        sp = doc.get('m_SavedProperties') or {}
        tex, fl = {}, {}
        for e in sp.get('m_TexEnvs') or []:
            for k, v in e.items():
                if v and (v.get('m_Texture') or {}).get('guid'):
                    tex[k] = v
        for e in sp.get('m_Floats') or []:
            fl.update(e)
        name = doc.get('m_Name') or os.path.splitext(os.path.basename(path))[0]
        foliage = fl.get('_ASSET_TYPE', 0) == 1 or fl.get('_MaterialTypeMask', 2) == 32 or 'billboard' in name.lower()
        out = os.path.join(os.path.dirname(pack.out_path(path, '.mat')), 'M_' + ui.tidy(name) + '.mat')
        if not os.path.exists(out):
            continue
        d = json.load(open(out, encoding='utf-8'))
        for k in [k for k in d['properties'] if k.startswith('_Cover')]:
            del d['properties'][k]
        if add_cover(pack, name, tex, fl, d['properties'], foliage):
            n += 1
            if not dry:
                write_json(out, d)
    if not dry:
        pack.write_texture_metas()
    print('materials with a cover layer:', n)


if __name__ == '__main__':
    main()
