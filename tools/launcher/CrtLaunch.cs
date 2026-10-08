// The CRT launch screen's native side (launch-crt.ps1): the tube shader's WPF effect, the art's
// text runs, the git line and a few Win32 calls. launch-crt.ps1 compiles this
// once into build\launcher-cache (keyed by a hash of this file) and loads it from there after.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Media.Effects;

public static class CrtLaunchNative {
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("kernel32.dll")] static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, int flags, IntPtr extra);

    public static void HideConsole() { IntPtr w = GetConsoleWindow(); if (w != IntPtr.Zero) ShowWindow(w, 0); }

    // Windows refuses the foreground to a window started in the background (the minimized
    // console's child), which would leave the Y / N keys going elsewhere. A tap of Alt is the
    // documented way to be allowed it.
    public static void TakeForeground(IntPtr hWnd) {
        keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero);
        SetForegroundWindow(hWnd);
    }
}

// The tube: crt.ps with its time and render size.
public class CrtEffect : ShaderEffect {
    public static readonly DependencyProperty InputProperty = ShaderEffect.RegisterPixelShaderSamplerProperty("Input", typeof(CrtEffect), 0);
    public static readonly DependencyProperty TimeProperty = DependencyProperty.Register("Time", typeof(double), typeof(CrtEffect),
        new UIPropertyMetadata(0.0, PixelShaderConstantCallback(0)));
    public static readonly DependencyProperty SizeProperty = DependencyProperty.Register("Size", typeof(Point), typeof(CrtEffect),
        new UIPropertyMetadata(new Point(1600, 1000), PixelShaderConstantCallback(1)));

    public CrtEffect(string shaderPath) {
        PixelShader shader = new PixelShader();
        using (FileStream stream = File.OpenRead(shaderPath)) shader.SetStreamSource(stream);
        PixelShader = shader;
        UpdateShaderValue(InputProperty); UpdateShaderValue(TimeProperty); UpdateShaderValue(SizeProperty);
    }
    public Brush Input { get { return (Brush)GetValue(InputProperty); } set { SetValue(InputProperty, value); } }
    public double Time { get { return (double)GetValue(TimeProperty); } set { SetValue(TimeProperty, value); } }
    public Point Size { get { return (Point)GetValue(SizeProperty); } set { SetValue(SizeProperty, value); } }
}

public static class CrtLaunchArt {
    // TartarusEngineAscii.txt's "::name" sections, each without its blank rows and common indent.
    public static Dictionary<string, string[]> ReadSections(string path) {
        var raw = new Dictionary<string, List<string>>();
        List<string> current = null;
        foreach (string line in File.ReadAllLines(path)) {
            if (line.StartsWith("::")) { current = new List<string>(); raw[line.Substring(2)] = current; continue; }
            if (current != null) current.Add(line.TrimEnd());
        }
        var result = new Dictionary<string, string[]>();
        foreach (var pair in raw) {
            List<string> lines = pair.Value;
            int first = 0, last = lines.Count - 1;
            while (first <= last && lines[first].Length == 0) first++;
            while (last >= first && lines[last].Length == 0) last--;
            int lead = int.MaxValue;
            for (int i = first; i <= last; i++)
                if (lines[i].Length > 0) lead = Math.Min(lead, lines[i].Length - lines[i].TrimStart().Length);
            if (lead == int.MaxValue) lead = 0;
            var block = new List<string>();
            for (int i = first; i <= last; i++) block.Add(lines[i].Length > lead ? lines[i].Substring(lead) : "");
            result[pair.Key] = block.ToArray();
        }
        return result;
    }

    static double Tone(char c) {
        switch (c) {
            case ' ': return -1; case '.': return 0.34; case ':': return 0.46; case '-': return 0.56; case '=': return 0.66;
            case '+': return 0.74; case '*': return 0.82; case '#': return 0.90; case '%': return 0.96; default: return 1.0;
        }
    }

    // The art as runs of equal density, each lit by its density, so the figure keeps its depth.
    public static void Fill(TextBlock target, string[] lines) {
        var brushes = new Dictionary<double, Brush>();
        for (int i = 0; i < lines.Length; i++) {
            string line = lines[i];
            int j = 0;
            while (j < line.Length) {
                double t = Tone(line[j]);
                int k = j + 1;
                while (k < line.Length && Tone(line[k]) == t) k++;
                var run = new Run(line.Substring(j, k - j));
                if (t >= 0) {
                    Brush b;
                    if (!brushes.TryGetValue(t, out b)) {
                        byte v = (byte)Math.Round(255 * t);
                        b = new SolidColorBrush(Color.FromRgb(v, v, (byte)Math.Min(255, v + 6)));
                        b.Freeze(); brushes[t] = b;
                    }
                    run.Foreground = b;
                }
                target.Inlines.Add(run);
                j = k;
            }
            if (i < lines.Length - 1) target.Inlines.Add(new LineBreak());
        }
    }
}

// "branch @ commit", read straight from .git (no git process): plain checkouts and worktrees.
public static class CrtLaunchGit {
    public static string Describe(string root) {
        try {
            string gitDir = Path.Combine(root, ".git");
            if (File.Exists(gitDir)) {                                   // a worktree: "gitdir: <path>"
                string line = File.ReadAllText(gitDir).Trim();
                if (!line.StartsWith("gitdir:")) return "";
                gitDir = Path.GetFullPath(Path.Combine(root, line.Substring(7).Trim()));
            }
            if (!Directory.Exists(gitDir)) return "";
            string commonDir = gitDir;
            string commonFile = Path.Combine(gitDir, "commondir");
            if (File.Exists(commonFile)) commonDir = Path.GetFullPath(Path.Combine(gitDir, File.ReadAllText(commonFile).Trim()));

            string head = File.ReadAllText(Path.Combine(gitDir, "HEAD")).Trim();
            if (!head.StartsWith("ref:")) return head.Length >= 7 ? head.Substring(0, 7) : head;   // detached
            string refName = head.Substring(4).Trim();
            string branch = refName.StartsWith("refs/heads/") ? refName.Substring(11) : refName;
            string sha = null;
            foreach (string dir in new[] { gitDir, commonDir }) {
                string refFile = Path.Combine(dir, refName.Replace('/', Path.DirectorySeparatorChar));
                if (File.Exists(refFile)) { sha = File.ReadAllText(refFile).Trim(); break; }
            }
            if (sha == null) {
                string packed = Path.Combine(commonDir, "packed-refs");
                if (File.Exists(packed))
                    foreach (string line in File.ReadAllLines(packed))
                        if (line.EndsWith(" " + refName)) { sha = line.Substring(0, line.IndexOf(' ')); break; }
            }
            return sha != null && sha.Length >= 7 ? branch + " @ " + sha.Substring(0, 7) : branch;
        } catch { return ""; }
    }
}

