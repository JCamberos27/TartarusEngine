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
