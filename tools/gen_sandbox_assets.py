# Generates the Sandbox scene's own assets: the basketball court (floor texture + wood detail
# maps, backboard glass), the basketball (albedo + pebble normal map, 64x32 sphere mesh), rim and
# net meshes, the materials (.mat: the court on engine://Standard.shader, Unity's Standard layout;
# the material gallery on engine://StandardAdvanced.shader for clear coat / sheen / transmission /
# anisotropy / subsurface), synthesized sounds (not used by the Sandbox itself; the
# smoke_play_basketball smoke scene plays them), and the Y Bot animator controller. Everything is
# procedural (PIL + the standard library only), so it can be re-run.
# Usage: python tools/gen_sandbox_assets.py <repo root>
import json, math, os, random, struct, sys, wave
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops, ImageOps

ROOT = sys.argv[1]
P = lambda *a: os.path.join(ROOT, 'project', *a)
for d in ('textures/basketball', 'models/basketball', 'materials/sandbox', 'sounds', 'animations'):
    os.makedirs(P(*d.split('/')), exist_ok=True)
random.seed(42)

# =============================================================================== court geometry
# NBA court, metres. The floor slab is the court plus a 2 m apron all round.
COURT_L, COURT_W = 28.65, 15.24
APRON = 2.0
FLOOR_L, FLOOR_W = COURT_L + 2 * APRON, COURT_W + 2 * APRON
LINE = 0.0508                 # 2"
BASKET_FROM_BASELINE = 1.60   # basket centre
KEY_W, KEY_L = 4.88, 5.79     # lane width, baseline to free-throw line
FT_R = 1.83                   # free-throw circle
THREE_R = 7.24                # arc radius
THREE_SIDE = 0.91             # corner three: distance of the straight part from the sideline
THREE_STRAIGHT = 4.27         # corner straights run this far from the baseline
RESTRICTED_R = 1.22
CENTER_R = 1.83
BOARD_W = 0.8 / 14            # floor board width (14 boards per 0.8 m detail tile)

TEX_W = 4096
PX = TEX_W / FLOOR_L          # pixels per metre
TEX_H = int(round(FLOOR_W * PX))

# The floor is one box; its top face maps u along +X and v along -Z (row 0 = the +Z / south
# edge). Draw in "court space" c = (x, z) metres from the court centre, x east, z south.
def to_px(x, z):
    return ((x + FLOOR_L / 2) * PX, (FLOOR_W / 2 - z) * PX)

# ------------------------------------------------------------------ wood
def make_floor():
    img = Image.new('RGB', (TEX_W, TEX_H))
    d = ImageDraw.Draw(img)
    rows = int(math.ceil(FLOOR_W / BOARD_W))
    base = (205, 160, 108)  # maple, sRGB
    for r in range(rows):
        y0, y1 = r * BOARD_W * PX, (r + 1) * BOARD_W * PX
        x = -random.uniform(0, 2.4)
        while x < FLOOR_L:
            length = random.uniform(1.1, 2.9)
            k = random.uniform(0.9, 1.08)
            warm = random.uniform(-6, 6)
            col = (int(min(255, base[0] * k + warm)), int(base[1] * k + warm * 0.4), int(base[2] * k - warm * 0.3))
            x0p, x1p = max(0, x) * PX, min(FLOOR_L, x + length) * PX
            if x1p > x0p:
                d.rectangle([x0p, y0, x1p, y1], fill=col)
            if x > 0:  # butt joint
                d.line([(x0p, y0), (x0p, y1)], fill=(150, 110, 70), width=1)
            x += length
        d.line([(0, y1), (TEX_W, y1)], fill=(172, 128, 82), width=1)  # board seam
    # Grain: noise stretched along the boards, and a softer large-scale figure.
    grain = Image.effect_noise((TEX_W // 24, TEX_H), 48).resize((TEX_W, TEX_H), Image.BILINEAR)
    figure = Image.effect_noise((TEX_W // 64, TEX_H // 8), 60).resize((TEX_W, TEX_H), Image.BICUBIC)
    shade = Image.blend(grain, figure, 0.4).convert('RGB')
    img = ImageChops.multiply(img, Image.eval(shade, lambda v: int(205 + v * 50 / 255)))
    return img

def ring_mask(draw, cx, cz, r, width, start=None, end=None, s=1.0):
    """Arc/circle line of court-space radius r (outer edge) and line width, on a mask."""
    px, py = to_px(cx, cz)
    ro = r * PX * s
    box = [px * s - ro, py * s - ro, px * s + ro, py * s + ro]
    w = max(1, int(round(width * PX * s)))
    if start is None:
        draw.ellipse(box, outline=255, width=w)
    else:
        draw.arc(box, start, end, fill=255, width=w)

def rect_line(draw, x0, z0, x1, z1, s=1.0):
    ax, ay = to_px(x0, z0); bx, by = to_px(x1, z1)
    draw.rectangle([min(ax, bx) * s, min(ay, by) * s, max(ax, bx) * s, max(ay, by) * s], fill=255)

def court_lines(s):
    """White line mask at `s` x supersampling."""
    m = Image.new('L', (int(TEX_W * s), int(TEX_H * s)), 0)
    d = ImageDraw.Draw(m)
    hl, hw = COURT_L / 2, COURT_W / 2
    L = LINE
    # Boundary (lines are inside the court), midcourt line.
    rect_line(d, -hl, -hw, hl, -hw + L, s); rect_line(d, -hl, hw - L, hl, hw, s)
    rect_line(d, -hl, -hw, -hl + L, hw, s); rect_line(d, hl - L, -hw, hl, hw, s)
    rect_line(d, -L / 2, -hw, L / 2, hw, s)
    ring_mask(d, 0, 0, CENTER_R, L, s=s)
    ring_mask(d, 0, 0, 0.61, L, s=s)  # inner centre circle
    for side in (-1, 1):
        bx = side * (hl - BASKET_FROM_BASELINE)          # basket centre x
        base_x = side * hl
        ft_x = side * (hl - KEY_L)
        # Lane
        x_a, x_b = sorted((base_x, ft_x))
        rect_line(d, x_a, -KEY_W / 2, x_b, -KEY_W / 2 + L, s)
        rect_line(d, x_a, KEY_W / 2 - L, x_b, KEY_W / 2, s)
        rect_line(d, ft_x - L / 2, -KEY_W / 2, ft_x + L / 2, KEY_W / 2, s)
        # Free-throw circle: solid half away from the basket (PIL angles: 0 = +x, clockwise on screen).
        if side == 1:
            ring_mask(d, ft_x, 0, FT_R, L, 90, 270, s)
        else:
            ring_mask(d, ft_x, 0, FT_R, L, 270, 90, s)
        # ...and the dashed half inside the lane.
        inner = (270, 90) if side == 1 else (90, 270)
        for k in range(8):
            a0 = inner[0] + k * 22.5 + 5.6
            ring_mask(d, ft_x, 0, FT_R, L, a0, a0 + 11.25, s)
        # Restricted area arc
        if side == 1:
            ring_mask(d, bx, 0, RESTRICTED_R, L, 90, 270, s)
        else:
            ring_mask(d, bx, 0, RESTRICTED_R, L, 270, 90, s)
        # Three-point line: straights, then the arc between them.
        zc = hw - THREE_SIDE
        xs = side * (hl - THREE_STRAIGHT)
        x_a, x_b = sorted((base_x, xs))
        rect_line(d, x_a, -zc, x_b, -zc + L, s)
        rect_line(d, x_a, zc - L, x_b, zc, s)
        ang = math.degrees(math.asin(zc / THREE_R))       # arc half-angle from the court axis
        if side == 1:
            ring_mask(d, bx, 0, THREE_R, L, 180 - ang, 180 + ang, s)
        else:
            ring_mask(d, bx, 0, THREE_R, L, -ang, ang, s)
        # Lane hash marks
        for off in (2.13, 3.05, 3.96):
            hx = side * (hl - off)
            for zz in (-KEY_W / 2 - 0.15, KEY_W / 2):
                rect_line(d, hx - L / 2, zz, hx + L / 2, zz + 0.15, s)
    return m.resize((TEX_W, TEX_H), Image.LANCZOS)

def paint_mask(s):
    """Areas painted in the team colour: the lanes and the centre circle."""
    m = Image.new('L', (int(TEX_W * s), int(TEX_H * s)), 0)
    d = ImageDraw.Draw(m)
    hl = COURT_L / 2
    for side in (-1, 1):
        x_a, x_b = sorted((side * hl, side * (hl - KEY_L)))
        rect_line(d, x_a, -KEY_W / 2, x_b, KEY_W / 2, s)
    px, py = to_px(0, 0)
    r = CENTER_R * PX * s
    d.ellipse([px * s - r, py * s - r, px * s + r, py * s + r], fill=255)
    return m.resize((TEX_W, TEX_H), Image.LANCZOS)

def apron_mask():
    m = Image.new('L', (TEX_W, TEX_H), 255)
    d = ImageDraw.Draw(m)
    ax, ay = to_px(-COURT_L / 2, COURT_W / 2)
    bx, by = to_px(COURT_L / 2, -COURT_W / 2)
    d.rectangle([ax, ay, bx, by], fill=0)
    return m

def font(size):
    for f in ('bahnschrift.ttf', 'impact.ttf', 'arialbd.ttf'):
        try:
            return ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:/Windows'), 'Fonts', f), size)
        except OSError:
            pass
    return ImageFont.load_default()

def text_patch(text, height_m, color, weight_font=None):
    f = weight_font or font(int(height_m * PX * 1.35))
    bbox = f.getbbox(text)
    w, h = bbox[2] - bbox[0] + 8, bbox[3] - bbox[1] + 8
    patch = Image.new('L', (w, h), 0)
    ImageDraw.Draw(patch).text((4 - bbox[0], 4 - bbox[1]), text, fill=255, font=f)
    return patch

def stamp(img, mask_patch, cx, cz, color, facing):
    """Composites text centred on court point (cx, cz), readable by a viewer standing on the
    `facing` side ('north' looks south, 'south' looks north)."""
    # A viewer on the north side sees the texture mirrored left-right; one on the south sees it
    # upside down (see to_px): pre-transform the patch so it reads correctly.
    patch = ImageOps.mirror(mask_patch) if facing == 'north' else ImageOps.flip(mask_patch)
    px, py = to_px(cx, cz)
    x0, y0 = int(px - patch.width / 2), int(py - patch.height / 2)
    layer = Image.new('RGB', patch.size, color)
    img.paste(layer, (x0, y0), patch)

def build_court():
    floor = make_floor()
    navy = Image.new('RGB', floor.size, (26, 44, 98))
    # Apron: darker walnut stain.
    stain = ImageChops.multiply(floor, Image.new('RGB', floor.size, (150, 104, 84)))
    floor = Image.composite(stain, floor, apron_mask())
    paint = paint_mask(2)
    floor = Image.composite(navy, floor, paint.point(lambda v: int(v * 0.86)))
    lines = court_lines(2)
    floor = Image.composite(Image.new('RGB', floor.size, (242, 240, 232)), floor, lines)
    # Centre logo: an orange ring and a big "T", read from the entrance (north) side.
    px, py = to_px(0, 0)
    d = ImageDraw.Draw(floor)
    r = 1.2 * PX
    d.ellipse([px - r, py - r, px + r, py + r], outline=(232, 120, 40), width=int(0.09 * PX))
    stamp(floor, text_patch('T', 1.2, None, font(int(1.45 * PX))), 0, 0.05, (232, 120, 40), 'north')
    # Apron lettering: south apron reads from the north, north apron from the south.
    stamp(floor, text_patch('TARTARUS ENGINE', 0.9, None), 0, COURT_W / 2 + APRON / 2, (238, 226, 200), 'north')
    stamp(floor, text_patch('TARTARUS ENGINE', 0.9, None), 0, -COURT_W / 2 - APRON / 2, (238, 226, 200), 'south')
    floor.save(P('textures', 'basketball', 'court_floor.jpg'), quality=90, optimize=True)

def build_floor_detail():
    # Neutral (50% grey) grain for the x2 detail albedo, and board-seam grooves as a detail
    # normal map. One tile = 0.8 m square = 14 boards, matching BOARD_W in the base texture.
    size = 512
    g = Image.effect_noise((size // 16, size), 30).resize((size, size), Image.BILINEAR)
    g = g.filter(ImageFilter.GaussianBlur(0.6))
    g = Image.eval(g, lambda v: int(128 + (v - 128) * 0.35))
    Image.merge('RGB', (g, g, g)).save(P('textures', 'basketball', 'wood_detail.png'))
    # Height: a shallow V-groove at each board edge (rows every size/14 px).
    h = Image.new('L', (size, size), 200)
    d = ImageDraw.Draw(h)
    per = size / 14
    for k in range(15):
        y = k * per
        d.line([(0, y), (size, y)], fill=60, width=2)
    h = h.filter(ImageFilter.GaussianBlur(1.2))
    normal_from_height(h, 2.0).save(P('textures', 'basketball', 'wood_seams_normal.png'))

def normal_from_height(h, strength):
    kx = ImageFilter.Kernel((3, 3), [-1, 0, 1, -2, 0, 2, -1, 0, 1], scale=8.0 / strength, offset=128)
    ky = ImageFilter.Kernel((3, 3), [-1, -2, -1, 0, 0, 0, 1, 2, 1], scale=8.0 / strength, offset=128)
    nx = ImageOps.invert(h.filter(kx))
    ny = h.filter(ky)
    nz = Image.new('L', h.size, 235)
    return Image.merge('RGB', (nx, ny, nz))

# =============================================================================== basketball
def build_ball():
    W, H = 2048, 1024
    alb = Image.new('RGB', (W, H))
    height = Image.new('L', (W, H), 160)
    apx = alb.load(); hpx = height.load()
    seam_w = 0.0055 / 0.1193  # seam half-width in radians of arc
    cz = math.cos(math.radians(52))
    base = (196, 86, 34)
    for j in range(H):
        th = (j + 0.5) / H * math.pi
        st, ct = math.sin(th), math.cos(th)
        for i in range(W):
            ph = (i + 0.5) / W * 2 * math.pi
            x, y, z = st * math.cos(ph), ct, st * math.sin(ph)
            # Seams: the equator (y = 0), the great circle x = 0, and the two curved seams - small
            # circles |x| = cz around the +/-x axis, which cross the equator but not x = 0.
            dist = min(abs(y), abs(x), abs(abs(x) - cz))
            if dist < seam_w:
                t = dist / seam_w
                apx[i, j] = (24, 20, 18)
                hpx[i, j] = int(40 + 90 * t * t)
            else:
                n = random.uniform(0.94, 1.04)
                apx[i, j] = (int(base[0] * n), int(base[1] * n), int(base[2] * n))
    # Pebbles: small bumps scattered evenly over the sphere (denser per pixel toward the poles,
    # where equirect texels are smaller on the ball, so they come out the same size on it).
    d = ImageDraw.Draw(height)
    for _ in range(90000):
        u = random.random()
        yv = random.uniform(-1, 1)          # uniform on the sphere
        th = math.acos(yv)
        j = th / math.pi * H
        i = u * W
        st = max(math.sin(th), 0.08)
        rx, ry = 2.2 / st, 2.2
        if hpx[int(i) % W, min(int(j), H - 1)] < 150:
            continue  # not in a seam
        d.ellipse([i - rx, j - ry, i + rx, j + ry], fill=205)
    height = height.filter(ImageFilter.GaussianBlur(0.9))
    alb = alb.filter(ImageFilter.GaussianBlur(0.4))
    alb.save(P('textures', 'basketball', 'basketball_albedo.png'))
    normal_from_height(height, 1.6).save(P('textures', 'basketball', 'basketball_normal.png'))

# =============================================================================== backboard
def build_backboard():
    W, H = 1024, 600          # 1.83 m x 1.07 m
    ppm = W / 1.83
    img = Image.new('RGBA', (W, H), (215, 235, 240, 46))
    d = ImageDraw.Draw(img)
    lw = int(0.05 * ppm)
    white = (250, 250, 250, 255)
    d.rectangle([0, 0, W - 1, lw], fill=white); d.rectangle([0, H - 1 - lw, W - 1, H - 1], fill=white)
    d.rectangle([0, 0, lw, H - 1], fill=white); d.rectangle([W - 1 - lw, 0, W - 1, H - 1], fill=white)
    # Target square 0.59 x 0.45, centred, its bottom line level with the rim (0.15 m above the
    # board's bottom edge). The box face maps v upward, so row 0 is the board's BOTTOM edge.
    sw, sh = 0.59 * ppm, 0.45 * ppm
    x0 = (W - sw) / 2
    y0 = 0.15 * ppm
    d.rectangle([x0, y0, x0 + sw, y0 + lw], fill=white)
    d.rectangle([x0, y0 + sh - lw, x0 + sw, y0 + sh], fill=white)
    d.rectangle([x0, y0, x0 + lw, y0 + sh], fill=white)
    d.rectangle([x0 + sw - lw, y0, x0 + sw, y0 + sh], fill=white)
    img.save(P('textures', 'basketball', 'backboard.png'))

# =============================================================================== meshes (OBJ)
def write_obj(path, verts, uvs, normals, faces, comment):
    with open(path, 'w', newline='\n') as f:
        f.write(f'# {comment}\n# Generated by tools/gen_sandbox_assets.py\n')
        for v in verts: f.write('v %.6f %.6f %.6f\n' % v)
        for t in uvs: f.write('vt %.6f %.6f\n' % t)
        for n in normals: f.write('vn %.6f %.6f %.6f\n' % n)
        for tri in faces:
            f.write('f ' + ' '.join('%d/%d/%d' % (a + 1, b + 1, c + 1) for a, b, c in tri) + '\n')

def build_sphere_obj():
    verts, uvs, norms, faces = [], [], [], []
    LON, LAT = 64, 32
    for j in range(LAT + 1):
        th = j / LAT * math.pi
        for i in range(LON + 1):
            ph = i / LON * 2 * math.pi
            n = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            verts.append(tuple(c * 0.5 for c in n)); norms.append(n)
            uvs.append((i / LON, 1.0 - j / LAT))  # the importer flips V: row 0 of the texture = north pole
    for j in range(LAT):
        for i in range(LON):
            a = j * (LON + 1) + i; b = a + 1; c = a + LON + 1; dd = c + 1
            if j != 0: faces.append([(a, a, a), (c, c, c), (b, b, b)])
            if j != LAT - 1: faces.append([(b, b, b), (c, c, c), (dd, dd, dd)])
    write_obj(P('models', 'basketball', 'basketball.obj'), verts, uvs, norms, faces,
              'Unit-diameter UV sphere (64 x 32), equirect UVs, for the basketball.')

def torus(major, minor, nu, nv):
    verts, uvs, norms, faces = [], [], [], []
    for i in range(nu + 1):
        u = i / nu * 2 * math.pi
        cu, su = math.cos(u), math.sin(u)
        for j in range(nv + 1):
            v = j / nv * 2 * math.pi
            n = (cu * math.cos(v), math.sin(v), su * math.cos(v))
            verts.append((cu * major + n[0] * minor, n[1] * minor, su * major + n[2] * minor))
            norms.append(n); uvs.append((i / nu, j / nv))
    for i in range(nu):
        for j in range(nv):
            a = i * (nv + 1) + j; b = a + 1; c = a + nv + 1; d = c + 1
            faces.append([(a, a, a), (b, b, b), (c, c, c)])
            faces.append([(b, b, b), (d, d, d), (c, c, c)])
    return verts, uvs, norms, faces

def build_rim_obj():
    # 18" (0.4572 m) inner diameter, 5/8" steel: tube radius ~0.009 m.
    v, t, n, f = torus(0.2286 + 0.009, 0.009, 64, 12)
    write_obj(P('models', 'basketball', 'rim.obj'), v, t, n, f,
              'Basketball rim: 0.457 m inner diameter, 18 mm steel tube. Origin at the ring centre.')

def build_net_obj():
    # Diamond-mesh net: 12 loops round, 6 rows, tapering from the rim to a narrower bottom.
    N, R = 12, 6
    length = 0.44
    def pt(i, j):
        a = 2 * math.pi * (i + 0.5 * (j % 2)) / N
        t = j / R
        r = 0.226 - 0.082 * (t ** 0.85)
        return (r * math.cos(a), -0.012 - length * t, r * math.sin(a))
    verts, uvs, norms, faces = [], [], [], []
    rad = 0.0042
    def strand(p, q):
        dx, dy, dz = q[0] - p[0], q[1] - p[1], q[2] - p[2]
        L = math.sqrt(dx * dx + dy * dy + dz * dz)
        ax = (dx / L, dy / L, dz / L)
        ref = (0, 1, 0) if abs(ax[1]) < 0.9 else (1, 0, 0)
        s1 = (ax[1] * ref[2] - ax[2] * ref[1], ax[2] * ref[0] - ax[0] * ref[2], ax[0] * ref[1] - ax[1] * ref[0])
        l1 = math.sqrt(sum(c * c for c in s1)); s1 = tuple(c / l1 for c in s1)
        s2 = (ax[1] * s1[2] - ax[2] * s1[1], ax[2] * s1[0] - ax[0] * s1[2], ax[0] * s1[1] - ax[1] * s1[0])
        K = 5
        base = len(verts)
        for end in (p, q):
            for k in range(K):
                a = 2 * math.pi * k / K
                nrm = tuple(math.cos(a) * s1[c] + math.sin(a) * s2[c] for c in range(3))
                verts.append(tuple(end[c] + nrm[c] * rad for c in range(3))); norms.append(nrm); uvs.append((k / K, 0))
        for k in range(K):
            a0, a1 = base + k, base + (k + 1) % K
            b0, b1 = a0 + K, a1 + K
            faces.append([(a0, a0, a0), (b0, b0, b0), (a1, a1, a1)])
            faces.append([(a1, a1, a1), (b0, b0, b0), (b1, b1, b1)])
    for j in range(R):
        for i in range(N):
            p = pt(i, j)
            if j % 2 == 0:
                strand(p, pt(i, j + 1)); strand(p, pt(i - 1, j + 1))
            else:
                strand(p, pt(i, j + 1)); strand(p, pt(i + 1, j + 1))
    # Top loop the net hangs from, just under the ring.
    for i in range(N):
        strand(pt(i, 0), pt(i + 1, 0))
    write_obj(P('models', 'basketball', 'net.obj'), verts, uvs, norms, faces,
              'Basketball net: diamond mesh, 12 loops x 6 rows, 0.44 m long. Origin at the rim centre.')

# =============================================================================== materials
def mat(name, file, props, queue=None, comment=None, opacity=None, advanced=False):
    m = {'matVersion': 2, 'name': name}
    if comment: m['_comment'] = comment
    m['shader'] = 'engine://StandardAdvanced.shader' if advanced else 'engine://Standard.shader'
    if queue is not None: m['renderQueue'] = queue
    if opacity is not None: m['opacity'] = opacity
    m['properties'] = props
    with open(P('materials', 'sandbox', file), 'w', newline='\n') as f:
        json.dump(m, f, indent=2)

def build_materials():
    T = 'textures/basketball/'
    # The scene lays this floor out at COURT_SCALE x the regulation size (gen_sandbox_scene.py's
    # COURT_S); the detail maps tile per 0.8 m of the scaled floor.
    COURT_SCALE = 1.5
    mat('Court Floor', 'court_floor.mat', {
        '_BaseColor': [1, 1, 1], '_Metallic': 0.0, '_Roughness': 0.62,
        '_AlbedoMap': T + 'court_floor.jpg',
        '_DetailAlbedoMap': T + 'wood_detail.png', '_DetailNormalMap': T + 'wood_seams_normal.png',
        '_DetailTiling': [FLOOR_L * COURT_SCALE / 0.8, FLOOR_W * COURT_SCALE / 0.8], '_ReflectionProbes': True},
        comment='Satin-finished maple: base court texture, x2 detail grain + board seams.')
    mat('Basketball', 'basketball.mat', {
        '_BaseColor': [1, 1, 1], '_Metallic': 0.0, '_Roughness': 0.85,
        '_AlbedoMap': T + 'basketball_albedo.png', '_NormalMap': T + 'basketball_normal.png',
        '_NormalStrength': 1.2, '_SpecularHighlights': False, '_GlossyReflections': False},
        comment='Pebbled composite leather: albedo with black seams, pebble normal map; fully matte '
                '(Specular Highlights and Reflections off, as on a Unity Standard material).')
    mat('Backboard Glass', 'backboard_glass.mat', {
        '_BaseColor': [1, 1, 1], '_Metallic': 0.0, '_Roughness': 0.04, '_AlbedoMap': T + 'backboard.png'},
        queue=2, comment='Tempered glass with painted border and target square (alpha in the texture).')
    mat('Arena Glass', 'arena_glass.mat', {
        '_BaseColor': [0.78, 0.9, 0.95], '_Metallic': 0.0, '_Roughness': 0.03},
        queue=2, opacity=0.13, comment='The arena walls and roof: clear, faintly blue-green glass.')
    mat('Rim Orange', 'rim.mat', {
        '_BaseColor': [0.9, 0.28, 0.04], '_Metallic': 0.2, '_Roughness': 0.5}, comment='Powder-coated steel.')
    mat('Net', 'net.mat', {
        '_BaseColor': [0.95, 0.95, 0.93], '_Metallic': 0.0, '_Roughness': 0.9}, comment='Nylon cord.')
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
    with wave.open(P('sounds', name), 'wb') as w:
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
    # Basketball bounce: a sharp slap, the ball's air-cavity "dong" and a short shell ring.
    n = int(SR * 0.45)
    slap = lowpass(noise(n), 2500)
    s = []
    for i in range(n):
        t = i / SR
        s.append(0.9 * math.exp(-t / 0.006) * slap[i] + env_sine(t, 265, 0.085) + env_sine(t, 590, 0.05, 0.35, 0.4)
                 + env_sine(t, 1480, 0.022, 0.18))
    write_wav('basketball_bounce.wav', fade_in(s, 0.6))
    # Rim clang: inharmonic steel partials.
    n = int(SR * 1.3)
    parts = [(510, 0.55, 1.0), (1330, 0.4, 0.7), (2270, 0.28, 0.5), (3390, 0.2, 0.33), (4470, 0.13, 0.2), (720, 0.3, 0.3)]
    hit = highpass(noise(n), 1500)
    s = [sum(env_sine(i / SR, f, dcy, a, f * 0.01) for f, dcy, a in parts) + 0.6 * math.exp(-i / SR / 0.004) * hit[i]
         for i in range(n)]
    write_wav('rim_clang.wav', fade_in(s, 0.3), 0.8)
    # Backboard: a deep glass thud with a faint high ring.
    n = int(SR * 0.6)
    body = lowpass(noise(n), 900)
    s = [env_sine(i / SR, 105, 0.07) + env_sine(i / SR, 232, 0.05, 0.6) + 0.8 * math.exp(-i / SR / 0.012) * body[i]
         + env_sine(i / SR, 1870, 0.18, 0.08) + env_sine(i / SR, 2950, 0.12, 0.05) for i in range(n)]
    write_wav('backboard_thud.wav', fade_in(s, 0.5))
    # Score: the net's swish, then a soft three-note chime.
    n = int(SR * 1.3)
    sw = highpass(lowpass(noise(n), 7000), 1800)
    s = []
    for i in range(n):
        t = i / SR
        e = (min(1.0, t / 0.03) * math.exp(-max(0.0, t - 0.03) / 0.16)) * (0.75 + 0.25 * math.sin(2 * math.pi * 23 * t))
        v = 0.9 * e * sw[i]
        for k, f in enumerate((784.0, 987.8, 1318.5)):  # G5 B5 E6
            t0 = 0.22 + 0.09 * k
            if t >= t0:
                tt = t - t0
                v += 0.22 * math.exp(-tt / 0.35) * (math.sin(2 * math.pi * f * tt) + 0.3 * math.sin(4 * math.pi * f * tt))
        s.append(v)
    write_wav('basket_score.wav', s, 0.85)
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

# =============================================================================== animator controller
def build_controller():
    Y = 'assets/characters/ybot/'
    c = {
        'defaultState': 'Idle',
        'parameters': [],
        'states': [
            {'name': 'Idle', 'clip': Y + 'idle.fbx', 'speed': 1.0, 'loop': True},
            {'name': 'Walk', 'clip': Y + 'walking.fbx', 'speed': 1.0, 'loop': True},
            {'name': 'Run', 'clip': Y + 'standard run.fbx', 'speed': 1.0, 'loop': True},
            {'name': 'Jump', 'clip': Y + 'jump.fbx', 'speed': 1.0, 'loop': False},
        ],
        'transitions': [
            {'from': 'Idle', 'to': 'Walk', 'hasExitTime': True, 'exitTime': 2.0, 'duration': 0.35, 'conditions': []},
            {'from': 'Walk', 'to': 'Run', 'hasExitTime': True, 'exitTime': 3.0, 'duration': 0.3, 'conditions': []},
            {'from': 'Run', 'to': 'Jump', 'hasExitTime': True, 'exitTime': 3.0, 'duration': 0.2, 'conditions': []},
            {'from': 'Jump', 'to': 'Idle', 'hasExitTime': True, 'exitTime': 0.92, 'duration': 0.3, 'conditions': []},
        ],
    }
    with open(P('animations', 'ybot_showcase.controller'), 'w', newline='\n') as f:
        json.dump(c, f, indent=2)

if __name__ == '__main__':
    steps = [('court', build_court), ('floor detail', build_floor_detail), ('ball', build_ball),
             ('backboard', build_backboard), ('sphere', build_sphere_obj), ('rim', build_rim_obj),
             ('net', build_net_obj), ('materials', build_materials), ('sounds', build_sounds),
             ('controller', build_controller)]
    only = set(sys.argv[2:])
    for name, fn in steps:
        if only and name not in only: continue
        print('building', name, flush=True)
        fn()
    print('done')
