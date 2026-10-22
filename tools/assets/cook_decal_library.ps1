# Cooks the decal library (project/assets/Decals) from the purchased texture packs, following
# tools/assets/decal_name_map.csv (written by gen_decal_name_map.ps1), and writes each cooked PNG's
# .meta and each decal's DA_<name>.decal next to it.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\cook_decal_library.ps1 `
#       -SourceRoot C:\...\decal_src -ProjectRoot <repo> [-DriveRoot "G:\My Drive\Tartarus Assets"] [-Force] [-DecalsOnly]
#
#   -SourceRoot  the packs as extracted (the folder the map's source_rel_path is relative to);
#                not needed with -DecalsOnly
#   -ProjectRoot the repo root; output goes to <ProjectRoot>\project\assets\Decals
#   -DriveRoot   optional: afterwards mirror the cooked PNGs to <DriveRoot>\Used\project\assets\Decals
#                (robocopy, PNGs only: .meta and .decal files live in git)
#   -Force       re-cook everything; otherwise a PNG newer than its source is kept
#   -Only        regex on dest_rel_path, to cook a subset (e.g. 'PeelingPaint_00[1-5]')
#   -Threads     worker threads (default: half the logical processors, max 12)
#   -Stats       print luminance percentiles of the listed sources instead of cooking (threshold tuning)
#   -Sheets      write a contact sheet per category into this folder (up to 48 per category, drawn
#                over a blue-grey checker so alpha is visible), after cooking
#   -DecalsOnly  only (re)write the DA_*.decal files of the rows that pass -Only: no texture cooking,
#                .meta writing, contact sheets or Drive mirror (a decal whose PNG is missing is skipped)
#   -Relief      also cook each peel decal's normal and surface maps (<stem>_N.png, <stem>_S.png,
#                both tiers; -Only and -Force apply). A decal whose _N/_S exist references them.
#
# Cooked format: PNG, RGBA 8-bit, straight alpha.
#   mask      ATS white-on-black sheets (alpha channel opaque, the mask is the luminance). 2048^2.
#             RGB = 255 so a tint works; A = remap(luminance, 3% -> 0 .. 100% -> 1): the noise floor
#             under 3% goes to 0, everything above keeps its anti-aliased ramp.
#   splatter  TW Vol 19 liquid splatter: as mask, times an edge vignette. 2048^2 and 4096^2 (_4K).
#   peel      TW Vol 1 peeling / cracked paint (light paint, dark = peeled / exposed). RGB = the grey
#             photo; A = smoothstep(PeelLo, PeelHi, 1 - luminance) times the edge vignette, so intact
#             paint is clear and flakes, cracks and exposed patches show. 2048^2 and 4096^2 (_4K).
#   grunge    peel without the vignette (tiles in the material Grunge slot). 4096^2, T_Grunge_*.
#   Vignette: per-axis smoothstep over the outer 6% of the width, multiplied (no hard box edge).
#   Peel thresholds: see $PeelLo / $PeelHi below and the note there.
#   relief    (peel, -Relief) a height field in millimetres over the decal's 2 m box, from the photo:
#               h = 0.35 * P1 + 0.5 * max(0, P1 - P3) - 0.5 * C + 0.6 * (L0.6 - L4)
#             P = paint (1 - the peel coverage), C = crack interiors (smoothstep(0.62, 0.85, 1 - lum)),
#             L = luminance, the suffix a Gaussian sigma in mm (so 2K and 4K match): the paint stands
#             0.35 mm proud with a soft 1 mm step, its edges curl up (the lip term), cracks sink and
#             the photo's fine grain adds texture.
#             _N  RGB = n * 0.5 + 0.5, n = normalize(-dh/dx, -dh/dup, 1) * gain 2 (Scharr), OpenGL
#                 green-up (+y = image top), flattened by the vignette; 24-bit opaque.
#             _S  r roughness 0.45 paint .. 0.85 plaster .. 0.95 cracks, g metal 0, b AO (cavity
#                 below its 2.5 mm neighbourhood and cracks, 0.5 .. 1), a height (-0.6 .. +0.8 mm).
#             Both linear (.meta isSRGB false; the normal's textureType 1, NormalMap).
#
# substrate: peel decals (Grime/PeelingPaint) carry a "substrate" block, the PBR material that shows
#   where the paint is missing; it defaults to Wall_Concrete (tiling 1.5 m, crop [0, 0.21, 1, 0.93]
#   to skip the sheet's trim bands, mirrored repeat). Its roughness map is empty: the
#   Wall_Concrete_Roughness.png in the pack is solid black, so roughnessRange [0.9, 0.9] stands in.
#
# .meta: the engine's texture meta (metaVersion 1, "type": "texture", importer block as AssetLibrary
# writes it): sRGB, compression 1 (Normal: BC3 for RGBA), mips, trilinear, aniso 8, maxTextureSize =
# tier, clamp for decals / repeat for grunge. GUIDs are minted the way AssetGuid::Generate does (a
# random non-zero 64-bit value, 16 hex digits) and an existing .meta keeps its GUID. .decal files get
# no .meta: the engine's AssetDatabase only gives known extensions one (".decal" isn't one).
param(
    [string]$SourceRoot,
    [Parameter(Mandatory)][string]$ProjectRoot,
    [string]$DriveRoot,
    [string]$MapPath,
    [switch]$Force,
    [string]$Only,
    [int]$Threads = 0,
    [switch]$Stats,
    [string]$Sheets,
    [switch]$Relief,
    [switch]$DecalsOnly
)
$ErrorActionPreference = 'Stop'
if (-not $SourceRoot -and -not $DecalsOnly) { throw '-SourceRoot is required (except with -DecalsOnly)' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $MapPath) { $MapPath = Join-Path $here 'decal_name_map.csv' }
if ($Threads -le 0) { $Threads = [Math]::Max(1, [Math]::Min(12, [int]([Environment]::ProcessorCount / 2))) }

# Mask tuning. ATS: luminance under MaskFloor is the sheets' noise floor.
$MaskFloor = 0.03
# Peeling paint: in the TW Vol 1 photos intact paint sits at luminance ~0.85-0.95 (the per-image
# median is 0.79-0.92), the exposed grey plaster / substrate at ~0.45-0.75, and crack interiors, flake
# shadows and holes at ~0.02-0.35. On darkness d = 1 - luminance, smoothstep(0.15, 0.40, d) keeps
# paint (luminance > 0.85) clear, makes everything darker than 0.60 opaque (all exposed substrate,
# cracks and shadows) and ramps the dirty-paint / flake-edge band in between. A first try at
# smoothstep(0.42, 0.62) only caught cracks and holes and dropped the exposed plaster.
# Picked from -Stats percentiles (TW 5, 15, ... 95, 100) and the contact sheets.
$PeelLo = 0.15
$PeelHi = 0.40
$VignetteFrac = 0.06

Add-Type -AssemblyName System.Drawing
if (-not ('TartarusDecalCook' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

public class TartarusDecalOut {
    public string Dst; public string Kind; public int Size;
}

public static class TartarusDecalCook {
    public static double MaskFloor = 0.03, PeelLo = 0.15, PeelHi = 0.40, Vignette = 0.06;

    static double Smooth(double e0, double e1, double x) {
        double t = (x - e0) / (e1 - e0);
        if (t < 0) t = 0; else if (t > 1) t = 1;
        return t * t * (3 - 2 * t);
    }

    // Source resized (bicubic, mirrored edges) to size x size, as BGRA bytes.
    static byte[] LoadBgra(Bitmap src, int size) {
        Bitmap bmp = src;
        bool own = false;
        if (src.Width != size || src.Height != size || src.PixelFormat != PixelFormat.Format32bppArgb) {
            bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            own = true;
            using (var g = Graphics.FromImage(bmp))
            using (var ia = new ImageAttributes()) {
                g.CompositingMode = CompositingMode.SourceCopy;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.SmoothingMode = SmoothingMode.None;
                ia.SetWrapMode(WrapMode.TileFlipXY);
                g.DrawImage(src, new Rectangle(0, 0, size, size), 0, 0, src.Width, src.Height, GraphicsUnit.Pixel, ia);
            }
        }
        try {
            var d = bmp.LockBits(new Rectangle(0, 0, size, size), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var buf = new byte[size * size * 4];
            for (int y = 0; y < size; y++) Marshal.Copy(d.Scan0 + y * d.Stride, buf, y * size * 4, size * 4);
            bmp.UnlockBits(d);
            return buf;
        } finally { if (own) bmp.Dispose(); }
    }

    // Rec.709 luma in 0..255, times the source alpha (opaque in these packs, but a real alpha must not
    // turn into an opaque mask).
    static int Luma(byte[] p, int i) {
        int l = (54 * p[i + 2] + 183 * p[i + 1] + 19 * p[i] + 128) >> 8;
        return (l * p[i + 3] + 127) / 255;
    }

    static float[] Ramp(int size, double frac) {
        var r = new float[size];
        double w = frac * size;
        for (int i = 0; i < size; i++) {
            double e = Math.Min(i + 0.5, size - i - 0.5);
            r[i] = w <= 0 ? 1f : (float)Smooth(0, w, e);
        }
        return r;
    }

    // bgra is size x size BGRA; rgb24 drops the alpha (an opaque map saves smaller as 24-bit RGB).
    static void Save(byte[] bgra, int size, string dst, bool rgb24 = false) {
        Directory.CreateDirectory(Path.GetDirectoryName(dst));
        string tmp = dst + ".tmp";
        var fmt = rgb24 ? PixelFormat.Format24bppRgb : PixelFormat.Format32bppArgb;
        using (var bmp = new Bitmap(size, size, fmt)) {
            var d = bmp.LockBits(new Rectangle(0, 0, size, size), ImageLockMode.WriteOnly, fmt);
            if (rgb24) {
                var row = new byte[size * 3];
                for (int y = 0; y < size; y++) {
                    for (int x = 0; x < size; x++) {
                        int s = (y * size + x) * 4;
                        row[x * 3] = bgra[s]; row[x * 3 + 1] = bgra[s + 1]; row[x * 3 + 2] = bgra[s + 2];
                    }
                    Marshal.Copy(row, 0, d.Scan0 + y * d.Stride, size * 3);
                }
            } else {
                for (int y = 0; y < size; y++) Marshal.Copy(bgra, y * size * 4, d.Scan0 + y * d.Stride, size * 4);
            }
            bmp.UnlockBits(d);
            bmp.Save(tmp, ImageFormat.Png);
        }
        if (File.Exists(dst)) File.Delete(dst);
        File.Move(tmp, dst);
    }

    // ---- Relief (peel): a height field from the photo, then a normal map and a surface map ----------
    // Physical scale: the decal is DecalMm wide (its default box), so a texel is DecalMm / size mm and
    // every blur radius and slope below is in millimetres: the 2K and 4K tiers come out the same.
    public static double DecalMm = 2000;
    public static double PaintMm = 0.35;   // intact paint stands this far above the exposed plaster
    public static double LipMm = 0.5;      // times the lip term (0..~0.5): flake edges curl up
    public static double CrackMm = 0.5;    // crack / hole interiors sink this far below the plaster
    public static double DetailMm = 0.6;   // times the photo's band-passed luminance (about +-0.15)
    public static double NormalGain = 2.0; // slope exaggeration for the normal map (1 = physical)
    public static double CrackLo = 0.62, CrackHi = 0.85; // darkness -> crack interior

    // Gaussian blur of sigma px (three extended-box passes per axis, fractional radius, clamped edges).
    static void Blur(float[] src, float[] dst, int n, double sigma) {
        if (sigma <= 0.05) { Array.Copy(src, dst, src.Length); return; }
        double w = Math.Sqrt(4 * sigma * sigma / 3.0 + 1); // 3 boxes: 3 * (w^2 - 1) / 12 = sigma^2
        double rr = (w - 1) / 2;
        int ri = (int)Math.Floor(rr);
        float fr = (float)(rr - ri);
        float inv = 1f / (2 * ri + 1 + 2 * fr);
        int pad = ri + 1, len = n + 2 * pad;
        var line = new float[len];
        var pre = new double[len + 1];
        var a = new float[src.Length];
        Array.Copy(src, a, src.Length);
        var b = dst;
        for (int axis = 0; axis < 2; axis++) {
            for (int pass = 0; pass < 3; pass++) {
                for (int k = 0; k < n; k++) {
                    for (int t = 0; t < len; t++) {
                        int s = Math.Min(n - 1, Math.Max(0, t - pad));
                        line[t] = axis == 0 ? a[k * n + s] : a[s * n + k];
                    }
                    pre[0] = 0;
                    for (int t = 0; t < len; t++) pre[t + 1] = pre[t] + line[t];
                    for (int x = 0; x < n; x++) {
                        int c = x + pad;
                        float v = (float)(pre[c + ri + 1] - pre[c - ri]) + fr * (line[c - ri - 1] + line[c + ri + 1]);
                        if (axis == 0) b[k * n + x] = v * inv; else b[x * n + k] = v * inv;
                    }
                }
                var t2 = a; a = b; b = t2;
            }
        }
        // 6 passes: the result is back in the array we started writing into first only if that is dst.
        if (!ReferenceEquals(a, dst)) Array.Copy(a, dst, a.Length);
    }

    // Writes <stem>_N.png (tangent-space normal, OpenGL green-up, RGB = n * 0.5 + 0.5, opaque) and
    // <stem>_S.png (r roughness, g metal 0, b AO, a height; linear).
    static void Relief(byte[] src, int n, string stem) {
        int N2 = n * n;
        double mm = DecalMm / n, px = 1.0 / mm; // texels per mm
        var lum = new float[N2];
        var cov = new float[N2];
        var crk = new float[N2];
        for (int i = 0; i < N2; i++) {
            float l = Luma(src, i * 4) / 255f;
            lum[i] = l;
            cov[i] = (float)Smooth(PeelLo, PeelHi, 1 - l);
            crk[i] = (float)Smooth(CrackLo, CrackHi, 1 - l);
        }
        var paint = new float[N2];
        for (int i = 0; i < N2; i++) paint[i] = 1 - cov[i];
        var pS = new float[N2]; Blur(paint, pS, n, 1.0 * px);   // the paint step, ~1 mm soft
        var pL = paint;          Blur(paint, pL, n, 3.0 * px);   // (in place) its neighbourhood
        var cS = cov;            Blur(crk, cS, n, 0.7 * px);     // crack depth, cov no longer needed
        var lS = crk;            Blur(lum, lS, n, 0.6 * px);
        var lB = lum;            Blur(lum, lB, n, 4.0 * px);     // in place: lum not needed after
        var h = new float[N2];
        for (int i = 0; i < N2; i++) {
            float lip = Math.Max(0f, pS[i] - pL[i]);             // paint side of a peel edge
            h[i] = (float)(PaintMm * pS[i] + LipMm * lip - CrackMm * cS[i] + DetailMm * (lS[i] - lB[i]));
        }
        var hB = pL; Blur(h, hB, n, 2.5 * px);                   // local mean height (AO)
        float[] ramp = Ramp(n, Vignette);
        var on = new byte[N2 * 4];
        var os = new byte[N2 * 4];
        double k = NormalGain / (32.0 * mm);                     // Scharr: 3-10-3 over 2 texels
        for (int y = 0; y < n; y++) {
            int ym = Math.Max(0, y - 1) * n, y0 = y * n, yp = Math.Min(n - 1, y + 1) * n;
            for (int x = 0; x < n; x++) {
                int xm = Math.Max(0, x - 1), xp = Math.Min(n - 1, x + 1);
                double gx = 3 * (h[ym + xp] - h[ym + xm]) + 10 * (h[y0 + xp] - h[y0 + xm]) + 3 * (h[yp + xp] - h[yp + xm]);
                double gy = 3 * (h[yp + xm] - h[ym + xm]) + 10 * (h[yp + x] - h[ym + x]) + 3 * (h[yp + xp] - h[ym + xp]);
                double v = ramp[x] * ramp[y];
                // dh/dx along +u; gy is dh/d(image down) so dh/d(up) = -gy: n = (-dh/dx, -dh/dup, 1).
                double nx = -gx * k * v, ny = gy * k * v, nz = 1;
                double inv = 1 / Math.Sqrt(nx * nx + ny * ny + nz * nz);
                int i = (y0 + x) * 4;
                on[i + 2] = (byte)Math.Round((nx * inv * 0.5 + 0.5) * 255);
                on[i + 1] = (byte)Math.Round((ny * inv * 0.5 + 0.5) * 255);
                on[i]     = (byte)Math.Round((nz * inv * 0.5 + 0.5) * 255);
                on[i + 3] = 255;
                int j = y0 + x;
                float c = 1 - pS[j];                                 // exposed plaster, soft-edged
                double rough = 0.45 + 0.40 * c;
                rough += (0.95 - rough) * cS[j];
                double cav = Math.Max(0.0, hB[j] - h[j]);            // below its neighbourhood: occluded
                double ao = Math.Min(1 - 1.25 * cav, 1 - 0.5 * cS[j]);
                ao = Math.Max(0.5, Math.Min(1.0, ao));
                ao = 1 - (1 - ao) * v;
                double ha = Math.Max(0.0, Math.Min(1.0, (h[j] + 0.6) / 1.4)); // -0.6 .. +0.8 mm
                os[i + 2] = (byte)Math.Round(rough * 255);
                os[i + 1] = 0;
                os[i]     = (byte)Math.Round(ao * 255);
                os[i + 3] = (byte)Math.Round(ha * 255);
            }
        }
        Save(on, n, stem + "_N.png", true);
        Save(os, n, stem + "_S.png");
    }

    static byte[] Cook(byte[] src, int size, string kind) {
        var lut = new double[256];
        for (int v = 0; v < 256; v++) {
            double l = v / 255.0;
            if (kind == "peel" || kind == "grunge") lut[v] = Smooth(PeelLo, PeelHi, 1 - l);
            else lut[v] = Math.Max(0, Math.Min(1, (l - MaskFloor) / (1 - MaskFloor)));
        }
        bool vignette = kind != "mask" && kind != "grunge";
        float[] ramp = vignette ? Ramp(size, Vignette) : null;
        bool grey = kind == "peel" || kind == "grunge";
        var o = new byte[size * size * 4];
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                int i = (y * size + x) * 4;
                int l = Luma(src, i);
                double a = lut[l];
                if (vignette) a *= ramp[x] * ramp[y];
                byte c = grey ? (byte)l : (byte)255;
                o[i] = c; o[i + 1] = c; o[i + 2] = c;
                o[i + 3] = (byte)Math.Round(a * 255);
            }
        }
        return o;
    }

    // One source -> all its outputs (decoded once). Returns log lines; failures are reported, not thrown.
    public static string[] Run(string[] sources, TartarusDecalOut[][] outs, int threads) {
        var log = new ConcurrentQueue<string>();
        Parallel.For(0, sources.Length, new ParallelOptions { MaxDegreeOfParallelism = threads }, k => {
            try {
                using (var img = new Bitmap(sources[k])) {
                    foreach (var grp in outs[k].GroupBy(o => o.Size)) {
                        byte[] px = LoadBgra(img, grp.Key);
                        foreach (var o in grp) {
                            if (o.Kind == "relief") Relief(px, grp.Key, o.Dst);
                            else Save(Cook(px, grp.Key, o.Kind), grp.Key, o.Dst);
                        }
                    }
                }
            } catch (Exception e) {
                log.Enqueue("FAILED " + sources[k] + ": " + e.Message);
            }
        });
        return log.ToArray();
    }

    // Luminance percentiles of each source at 1024^2, plus its minimum alpha.
    public static string Stats(string path) {
        using (var img = new Bitmap(path)) {
            byte[] px = LoadBgra(img, 1024);
            var h = new long[256];
            int minA = 255;
            for (int i = 0; i < px.Length; i += 4) {
                h[(54 * px[i + 2] + 183 * px[i + 1] + 19 * px[i] + 128) >> 8]++;
                minA = Math.Min(minA, px[i + 3]);
            }
            double[] qs = { 0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99 };
            long n = px.Length / 4, acc = 0;
            var res = new List<string>();
            int q = 0;
            for (int v = 0; v < 256 && q < qs.Length; v++) {
                acc += h[v];
                while (q < qs.Length && acc >= qs[q] * n) { res.Add((v / 255.0).ToString("0.00")); q++; }
            }
            return string.Join(" ", res) + "  minA=" + minA;
        }
    }

    // Contact sheet: tiles over a blue-grey checker so the alpha reads; labels under each tile.
    public static void Sheet(string[] files, string[] labels, string outPath, int cell, int cols) {
        int rows = (files.Length + cols - 1) / cols, lab = 16;
        using (var bmp = new Bitmap(cols * cell, Math.Max(1, rows) * (cell + lab), PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp))
        using (var font = new Font("Segoe UI", 8))
        using (var c1 = new SolidBrush(Color.FromArgb(72, 92, 120)))
        using (var c2 = new SolidBrush(Color.FromArgb(96, 116, 144))) {
            g.Clear(Color.FromArgb(30, 30, 30));
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.CompositingMode = CompositingMode.SourceOver;
            for (int k = 0; k < files.Length; k++) {
                int x = (k % cols) * cell, y = (k / cols) * (cell + lab);
                const int sq = 16;
                for (int cy = 0; cy < cell; cy += sq)
                    for (int cx = 0; cx < cell; cx += sq)
                        g.FillRectangle((((cx + cy) / sq) & 1) == 0 ? c1 : c2, x + cx, y + cy,
                                        Math.Min(sq, cell - cx), Math.Min(sq, cell - cy));
                using (var img = new Bitmap(files[k])) g.DrawImage(img, x + 2, y + 2, cell - 4, cell - 4);
                g.DrawString(labels[k], font, Brushes.Yellow, x + 2, y + cell);
            }
            bmp.Save(outPath, ImageFormat.Png);
        }
    }
}
'@
}
[TartarusDecalCook]::MaskFloor = $MaskFloor
[TartarusDecalCook]::PeelLo = $PeelLo
[TartarusDecalCook]::PeelHi = $PeelHi
[TartarusDecalCook]::Vignette = $VignetteFrac

$decalsRoot = Join-Path $ProjectRoot 'project\assets\Decals'
$rows = @(Import-Csv -LiteralPath $MapPath)
if ($Only) { $rows = @($rows | Where-Object { $_.dest_rel_path -match $Only }) }
if (-not $rows) { throw "No rows in $MapPath$(if ($Only) { " match '$Only'" })" }

if ($Stats) {
    $rows | Select-Object -ExpandProperty source_rel_path -Unique | ForEach-Object {
        '{0,-60} {1}' -f (Split-Path $_ -Leaf), [TartarusDecalCook]::Stats((Join-Path $SourceRoot $_))
    }
    '(columns: luminance at the 1/5/10/25/50/75/90/95/99th percentile)'
    return
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$texTime = $sw.Elapsed

# ---- Textures -------------------------------------------------------------------------------------
if (-not $DecalsOnly) {
$todo = [ordered]@{}
$skipped = 0
foreach ($r in $rows) {
    $src = Join-Path $SourceRoot $r.source_rel_path
    $dst = Join-Path $decalsRoot ($r.dest_rel_path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $src)) { throw "Missing source: $src" }
    if (-not $Force -and (Test-Path -LiteralPath $dst) -and
        (Get-Item -LiteralPath $dst).LastWriteTimeUtc -gt (Get-Item -LiteralPath $src).LastWriteTimeUtc) {
        $skipped++; continue
    }
    if (-not $todo.Contains($src)) { $todo[$src] = New-Object System.Collections.Generic.List[TartarusDecalOut] }
    $o = New-Object TartarusDecalOut
    $o.Dst = $dst; $o.Kind = $r.kind; $o.Size = [int]$r.tier
    $todo[$src].Add($o)
}
# Relief maps (separate freshness check: a fresh albedo can still need its _N / _S).
$reliefCount = 0
if ($Relief) {
    foreach ($r in $rows) {
        if ($r.kind -ne 'peel') { continue }
        $src = Join-Path $SourceRoot $r.source_rel_path
        $stem = (Join-Path $decalsRoot ($r.dest_rel_path -replace '/', '\')) -replace '\.png$', ''
        $srcTime = (Get-Item -LiteralPath $src).LastWriteTimeUtc
        $fresh = -not $Force -and @("${stem}_N.png", "${stem}_S.png" | Where-Object {
            -not (Test-Path -LiteralPath $_) -or (Get-Item -LiteralPath $_).LastWriteTimeUtc -le $srcTime }).Count -eq 0
        if ($fresh) { $skipped++; continue }
        if (-not $todo.Contains($src)) { $todo[$src] = New-Object System.Collections.Generic.List[TartarusDecalOut] }
        $o = New-Object TartarusDecalOut
        $o.Dst = $stem; $o.Kind = 'relief'; $o.Size = [int]$r.tier
        $todo[$src].Add($o)
        $reliefCount++
    }
    "Relief: $reliefCount normal + surface pair(s) to cook."
}
$outCount = ($todo.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
"Cooking $outCount texture(s) from $($todo.Count) source(s) on $Threads thread(s); $skipped up to date."
if ($todo.Count) {
    $srcs = [string[]]@($todo.Keys)
    $outs = [TartarusDecalOut[][]]@($todo.Values | ForEach-Object { , $_.ToArray() })
    $failed = [TartarusDecalCook]::Run($srcs, $outs, $Threads)
    $failed | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    if ($failed) { throw "$($failed.Count) source(s) failed" }
}
$texTime = $sw.Elapsed
}

# ---- .meta and .decal -----------------------------------------------------------------------------
$utf8 = New-Object Text.UTF8Encoding $false
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
function New-Guid64 {
    $b = New-Object byte[] 8
    do { $rng.GetBytes($b); $v = [BitConverter]::ToUInt64($b, 0) } while ($v -eq 0)
    $v.ToString('x16')
}
function Write-IfChanged([string]$path, [string]$text) {
    if ((Test-Path -LiteralPath $path) -and [IO.File]::ReadAllText($path) -ceq $text) { return $false }
    [IO.File]::WriteAllText($path, $text, $utf8); return $true
}
# JSON as nlohmann's dump(2) lays it out (2-space indent, arrays one element per line).
function Fmt($v) {
    if ($v -is [bool]) { return $(if ($v) { 'true' } else { 'false' }) }
    if ($v -is [string]) { return '"' + ($v -replace '\\', '\\' -replace '"', '\"') + '"' }
    if ($v -is [int]) { return $v.ToString([Globalization.CultureInfo]::InvariantCulture) }
    $s = ([double]$v).ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    if ($s -notmatch '[.eE]') { $s += '.0' }
    return $s
}
function To-Json($v, [int]$ind = 0) {
    $pad = ' ' * ($ind + 2); $end = ' ' * $ind
    if ($v -is [System.Collections.Specialized.OrderedDictionary]) {
        if ($v.Count -eq 0) { return '{}' }
        $parts = foreach ($k in $v.Keys) { $pad + '"' + $k + '": ' + (To-Json $v[$k] ($ind + 2)) }
        return "{`n" + ($parts -join ",`n") + "`n$end}"
    }
    if ($v -is [array]) {
        if ($v.Count -eq 0) { return '[]' }
        $parts = foreach ($e in $v) { $pad + (To-Json $e ($ind + 2)) }
        return "[`n" + ($parts -join ",`n") + "`n$end]"
    }
    return Fmt $v
}

function Get-Tags([string]$category) {
    $words = foreach ($seg in $category -split '/') { [regex]::Matches($seg, '[A-Z][a-z]*|[a-z]+|\d+') | ForEach-Object { $_.Value.ToLowerInvariant() } }
    , @($words | Select-Object -Unique)
}

# Per-category defaults (see docs/PROJECT_ASSETS.md, "Decals").
function Get-DecalJson($r) {
    $cat = $r.category
    $tier = [int]$r.tier
    $tex = 'assets/Decals/' + $r.dest_rel_path
    $ch = [ordered]@{ albedo = 1.0; normal = 0.0; roughness = 0.0; metal = 0.0; ao = 0.0; emissive = 0.0 }
    $texN = ''; $texS = ''
    $base = @(1.0, 1.0, 1.0); $rough = 0.6; $size = @(1.0, 1.0, 0.25)
    $edge = 0.08; $hb = 0.0
    $paint = [ordered]@{ enabled = $false; roughness = 0.35; metal = 0.0; relief = 0.3; uv = $false }
    switch -Regex ($cat) {
        '^Graffiti/' {
            $ch.roughness = 1.0; $rough = 0.6; $hb = 0.35
            $paint.enabled = $true; $paint.roughness = 0.45; $paint.relief = 0.15
            if ($cat -match '^Graffiti/(Alphabets|Letters|Numbers)') { $size = @(0.6, 0.6, 0.25) }
        }
        '^WarPaint$' {
            $ch.roughness = 1.0; $rough = 0.6; $hb = 0.35
            $paint.enabled = $true; $paint.roughness = 0.45; $paint.relief = 0.15
        }
        '^Grime/PeelingPaint$' {
            $ch.roughness = 1.0; $rough = 0.4; $ch.ao = 0.5; $edge = 0.15
            $size = @(2.0, 2.0, 0.25)
            $stemPath = (Join-Path $decalsRoot ($r.dest_rel_path -replace '/', [string][char]92)) -replace '\.png$', ''
            if ((Test-Path -LiteralPath "${stemPath}_N.png") -and (Test-Path -LiteralPath "${stemPath}_S.png")) {
                $ch.normal = 1.0; $ch.ao = 1.0; $hb = 0.4
                $texN = $tex -replace '\.png$', '_N.png'; $texS = $tex -replace '\.png$', '_S.png'
            }
        }
        '^Stains/LiquidSplatter$' {
            $ch.roughness = 1.0; $rough = 0.5; $hb = 0.5; $base = @(0.85, 0.85, 0.85)
            $size = @(1.5, 1.5, 0.25)
        }
        '^Stains/Splatters$' {
            $ch.roughness = 1.0; $rough = 0.5; $base = @(0.25, 0.25, 0.25)
        }
        '^(Signs|Stamps)$' {
            $paint.enabled = $true
            $size = @(0.5, 0.5, 0.25)
        }
    }
    $j = [ordered]@{
        decalVersion   = 1
        textures       = [ordered]@{ albedo = $tex; normal = $texN; surface = $texS; aux = '' }
        lumaMask       = $false
        invertMask     = $false
        tier           = $tier
        atlas          = [ordered]@{ cols = 1; rows = 1 }
        defaultSize    = $size
        channels       = $ch
        baseColor      = $base
        roughness      = $rough
        metal          = 0.0
        emissive       = @(0.0, 0.0, 0.0)
        angleFade      = @(0.25, 0.05)
        edgeFade       = $edge
        heightBlend    = $hb
        heightContrast = 4.0
        paint          = $paint
        weathering     = [ordered]@{ fade = 0.0; bleach = 0.0; peel = 0.0; buff = 0.0 }
        category       = $cat
        tags           = (Get-Tags $cat)
    }
    if ($cat -match '^Grime/PeelingPaint$') {
        # Wall_Concrete substrate; the pack's roughness PNG is solid black, so none (roughnessRange stands in).
        $sub = 'assets/Architecture/Building/Textures/Wall_Concrete/Wall_Concrete_'
        $j.Insert(($j.Keys.Count - 2), 'substrate', [ordered]@{
            albedo         = $sub + 'Base_Color.png'
            normal         = $sub + 'Normal.png'
            roughness      = ''
            ao             = $sub + 'AO.png'
            tiling         = 1.5
            tint           = @(1.0, 1.0, 1.0)
            strength       = 1.0
            roughnessRange = @(0.9, 0.9)
            mirror         = $true
            crop           = @(0.0, 0.21, 1.0, 0.93)
        })
    }
    (To-Json $j) + "`n"
}

$allRows = @(Import-Csv -LiteralPath $MapPath)
$metaNew = 0; $metaKept = 0; $decalWritten = 0; $decalCount = 0
foreach ($r in $allRows) {
    if ($Only -and $r.dest_rel_path -notmatch $Only) { continue }
    $png = Join-Path $decalsRoot ($r.dest_rel_path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $png)) { continue }
    $dir = Split-Path $png

    # .meta: keep an existing GUID (and never rewrite an unchanged file).
    $meta = "$png.meta"
    $guid = $null
    if (Test-Path -LiteralPath $meta) {
        $m = [regex]::Match([IO.File]::ReadAllText($meta), '"guid"\s*:\s*"([0-9a-fA-F]{16})"')
        if ($m.Success) { $guid = $m.Groups[1].Value.ToLowerInvariant() }
    }
    if ($guid) { $metaKept++ } else { $guid = New-Guid64; $metaNew++ }
    $folder = 'assets/Decals/' + ($r.dest_rel_path.Substring(0, $r.dest_rel_path.LastIndexOf('/')))
    $wrap = if ($r.kind -eq 'grunge') { 0 } else { 1 }
    $metaJson = [ordered]@{
        folder      = $folder
        guid        = $guid
        importer    = [ordered]@{
            anisoLevel = 8; compression = 1; filterMode = 2; generateMipmaps = $true; isSRGB = $true
            maxTextureSize = [int]$r.tier; textureType = 0; wrapMode = $wrap
        }
        metaVersion = 1
        type        = 'texture'
    }
    if (-not $DecalsOnly) { [void](Write-IfChanged $meta (To-Json $metaJson)) }

    # Relief maps: linear metas (normal map type for _N), GUIDs kept.
    if ($r.kind -eq 'peel' -and -not $DecalsOnly) {
        foreach ($suf in '_N', '_S') {
            $rp = ($png -replace '\.png$', '') + $suf + '.png'
            if (-not (Test-Path -LiteralPath $rp)) { continue }
            $rm = "$rp.meta"; $rg = $null
            if (Test-Path -LiteralPath $rm) {
                $m2 = [regex]::Match([IO.File]::ReadAllText($rm), '"guid"\s*:\s*"([0-9a-fA-F]{16})"')
                if ($m2.Success) { $rg = $m2.Groups[1].Value.ToLowerInvariant() }
            }
            if ($rg) { $metaKept++ } else { $rg = New-Guid64; $metaNew++ }
            $rj = [ordered]@{
                folder      = $folder
                guid        = $rg
                importer    = [ordered]@{
                    anisoLevel = 8; compression = 1; filterMode = 2; generateMipmaps = $true; isSRGB = $false
                    maxTextureSize = [int]$r.tier; textureType = $(if ($suf -eq '_N') { 1 } else { 0 }); wrapMode = $wrap
                }
                metaVersion = 1
                type        = 'texture'
            }
            [void](Write-IfChanged $rm (To-Json $rj))
        }
    }

    if ($r.kind -ne 'grunge') {
        $stem = [IO.Path]::GetFileNameWithoutExtension($png)
        $decal = Join-Path $dir ('DA_' + $stem.Substring('T_Decal_'.Length) + '.decal')
        if (Write-IfChanged $decal (Get-DecalJson $r)) { $decalWritten++ }
        $decalCount++
    }
}
"Metas: $metaNew new, $metaKept kept. Decals: $decalCount ($decalWritten written)."
"Textures took $([Math]::Round($texTime.TotalSeconds, 1)) s; total $([Math]::Round($sw.Elapsed.TotalSeconds, 1)) s."

# ---- Contact sheets -------------------------------------------------------------------------------
if ($Sheets -and -not $DecalsOnly) {
    New-Item -ItemType Directory -Force $Sheets | Out-Null
    $byCat = $allRows | Group-Object { if ($_.kind -eq 'grunge') { "$($_.category) (grunge)" } elseif ($_.tier -eq '4096') { "$($_.category) (4K)" } else { $_.category } }
    foreach ($g in $byCat) {
        $items = @($g.Group | Where-Object { Test-Path -LiteralPath (Join-Path $decalsRoot ($_.dest_rel_path -replace '/', '\')) })
        if (-not $items) { continue }
        if ($items.Count -gt 48) {
            $step = $items.Count / 48.0
            $items = @(0..47 | ForEach-Object { $items[[int][Math]::Floor($_ * $step)] })
        }
        $files = [string[]]@($items | ForEach-Object { Join-Path $decalsRoot ($_.dest_rel_path -replace '/', '\') })
        $labels = [string[]]@($items | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.dest_rel_path) -replace '^T_(Decal|Grunge)_', '' })
        $name = ($g.Name -replace '[/ ]', '_' -replace '[()]', '') + '.png'
        [TartarusDecalCook]::Sheet($files, $labels, (Join-Path $Sheets $name), 160, 8)
        "sheet $(Join-Path $Sheets $name) ($($files.Count))"
    }
}

# ---- Drive ----------------------------------------------------------------------------------------
if ($DriveRoot -and -not $DecalsOnly) {
    $dst = Join-Path $DriveRoot 'Used\project\assets\Decals'
    robocopy $decalsRoot $dst *.png /E /MT:16 /R:3 /W:5 /NP /NDL /NFL /XF *.tmp | Select-Object -Last 12
    if ($LASTEXITCODE -ge 8) { throw "robocopy to $dst failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}
