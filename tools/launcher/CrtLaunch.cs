// The CRT launch screen's native side (launch-crt.ps1): the tube shader's WPF effect, the art's
// text runs, the git line, the tube's sounds and a few Win32 calls. launch-crt.ps1 compiles this
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

// The tube's sounds, synthesized (no audio assets) and mixed live into a waveOut stream: the relay
// clunk and degauss swell of power on, the high-voltage whine and mains hum while it's lit (a
// seamless loop that fades), and the collapse of power off. Any audio failure just means silence.
public sealed class CrtLaunchSound : IDisposable {
    const int Rate = 44100;
    const int BlockSamples = 1024, Blocks = 4;             // ~23 ms blocks, ~90 ms of latency

    [StructLayout(LayoutKind.Sequential)] struct WAVEFORMATEX {
        public ushort FormatTag, Channels; public uint SamplesPerSec, AvgBytesPerSec; public ushort BlockAlign, BitsPerSample, Size;
    }
    [StructLayout(LayoutKind.Sequential)] struct WAVEHDR {
        public IntPtr Data; public uint BufferLength, BytesRecorded; public IntPtr User; public uint Flags, Loops; public IntPtr Next, Reserved;
    }
    [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr device, IntPtr id, ref WAVEFORMATEX format, IntPtr callback, IntPtr instance, uint flags);
    [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr device, IntPtr header, int size);
    [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr device, IntPtr header, int size);
    [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr device, IntPtr header, int size);
    [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr device);
    [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr device);

    sealed class Voice { public float[] Data; public int Position; public bool Loop; public float Gain; }

    readonly object gate = new object();
    readonly List<Voice> voices = new List<Voice>();
    float[] on, hum, off;
    Voice humVoice;
    float humLevel, humTarget, master = 1, masterTarget = 1;
    IntPtr device = IntPtr.Zero;
    System.Threading.Thread thread;
    volatile bool running;
    public int OpenResult = -1;   // waveOutOpen's result (0 = open); for diagnostics

    // Synthesizes the sounds and opens the default output, on its own thread (a few tens of ms).
    public void Start() {
        running = true;
        thread = new System.Threading.Thread(Run) { IsBackground = true, Priority = System.Threading.ThreadPriority.AboveNormal };
        thread.Start();
    }
    public void PowerOnSound() { Add(() => on, false, 0.8f); }
    public void PowerOffSound() { Add(() => off, false, 0.9f); }
    public void SetHum(float level) { lock (gate) humTarget = Math.Max(0, Math.Min(1, level)); }
    public void SetMuted(bool muted) { lock (gate) masterTarget = muted ? 0 : 1; }

    readonly List<KeyValuePair<Func<float[]>, float>> pending = new List<KeyValuePair<Func<float[]>, float>>();
    void Add(Func<float[]> sound, bool loop, float gain) {
        lock (gate) {
            if (sound() == null) { pending.Add(new KeyValuePair<Func<float[]>, float>(sound, gain)); return; }   // still synthesizing
            voices.Add(new Voice { Data = sound(), Loop = loop, Gain = gain });
        }
    }

    void Run() {
        IntPtr[] headers = new IntPtr[Blocks];
        IntPtr[] buffers = new IntPtr[Blocks];
        int headerSize = Marshal.SizeOf(typeof(WAVEHDR));
        try {
            float[] a = PowerOn(), b = Hum(), c = PowerOff();
            lock (gate) {
                on = a; hum = b; off = c;
                humVoice = new Voice { Data = hum, Loop = true, Gain = 0.55f };
                voices.Add(humVoice);
                foreach (var p in pending) voices.Add(new Voice { Data = p.Key(), Gain = p.Value });
                pending.Clear();
            }
            var format = new WAVEFORMATEX { FormatTag = 1, Channels = 1, SamplesPerSec = Rate, AvgBytesPerSec = Rate * 2, BlockAlign = 2, BitsPerSample = 16 };
            OpenResult = waveOutOpen(out device, new IntPtr(-1), ref format, IntPtr.Zero, IntPtr.Zero, 0);
            if (OpenResult != 0) { device = IntPtr.Zero; return; }
            for (int i = 0; i < Blocks; i++) {
                buffers[i] = Marshal.AllocHGlobal(BlockSamples * 2);
                headers[i] = Marshal.AllocHGlobal(headerSize);
                var h = new WAVEHDR { Data = buffers[i], BufferLength = BlockSamples * 2, Flags = 0 };
                Marshal.StructureToPtr(h, headers[i], false);
                waveOutPrepareHeader(device, headers[i], headerSize);
                Mix(buffers[i]);
                waveOutWrite(device, headers[i], headerSize);
            }
            int flagsOffset = (int)Marshal.OffsetOf(typeof(WAVEHDR), "Flags");
            while (running) {
                bool wrote = false;
                for (int i = 0; i < Blocks; i++) {
                    if ((Marshal.ReadInt32(headers[i], flagsOffset) & 1) == 0) continue;   // WHDR_DONE
                    Mix(buffers[i]);
                    waveOutWrite(device, headers[i], headerSize);
                    wrote = true;
                }
                if (!wrote) System.Threading.Thread.Sleep(4);
            }
        } catch { }
        finally {
            if (device != IntPtr.Zero) {
                waveOutReset(device);
                for (int i = 0; i < Blocks; i++)
                    if (headers[i] != IntPtr.Zero) waveOutUnprepareHeader(device, headers[i], headerSize);
                waveOutClose(device);
            }
            for (int i = 0; i < Blocks; i++) {
                if (headers[i] != IntPtr.Zero) Marshal.FreeHGlobal(headers[i]);
                if (buffers[i] != IntPtr.Zero) Marshal.FreeHGlobal(buffers[i]);
            }
        }
    }

    readonly short[] block = new short[BlockSamples];
    void Mix(IntPtr buffer) {
        lock (gate) {
            for (int n = 0; n < BlockSamples; n++) {
                // Smooth level changes: ~60 ms for the hum, ~25 ms for mute.
                humLevel += (humTarget - humLevel) * 0.0004f;
                master += (masterTarget - master) * 0.001f;
                float sum = 0;
                for (int v = voices.Count - 1; v >= 0; v--) {
                    Voice voice = voices[v];
                    float g = voice == humVoice ? voice.Gain * humLevel : voice.Gain;
                    sum += voice.Data[voice.Position] * g;
                    if (++voice.Position >= voice.Data.Length) {
                        if (voice.Loop) voice.Position = 0; else voices.RemoveAt(v);
                    }
                }
                block[n] = (short)(Math.Max(-1f, Math.Min(1f, sum * master)) * 32000);
            }
        }
        Marshal.Copy(block, 0, buffer, BlockSamples);
    }

    public void Dispose() {
        running = false;
        if (thread != null) thread.Join(500);
    }

    static double Smooth(double a, double b, double x) { double t = Math.Max(0, Math.Min(1, (x - a) / (b - a))); return t * t * (3 - 2 * t); }
    const double Tau = Math.PI * 2;

    static float[] PowerOn() {
        var rnd = new Random(7);
        int n = (int)(Rate * 1.8);
        var s = new float[n];
        double low = 0, crackle = 0;
        for (int i = 0; i < n; i++) {
            double t = i / (double)Rate;
            // The relay: a low thud with a short burst of filtered noise.
            low += (rnd.NextDouble() * 2 - 1 - low) * 0.08;
            double clunk = Math.Sin(Tau * 48 * t) * Math.Exp(-t * 14) * 0.55 + low * Math.Exp(-t * 55) * 1.4;
            // The degauss coil: a buzzing swell that dies away with a slow wobble.
            double swell = Smooth(0.03, 0.14, t) * Math.Exp(-Math.Max(0, t - 0.14) * 2.4);
            double buzz = (Math.Sin(Tau * 100 * t) + 0.55 * Math.Sin(Tau * 200 * t) + 0.3 * Math.Sin(Tau * 300 * t) + 0.35 * Math.Sin(Tau * 50 * t))
                          * swell * 0.2 * (1 + 0.35 * Math.Sin(Tau * 6.5 * t));
            // Static as the charge builds on the glass.
            if (t < 1.0 && rnd.NextDouble() < 0.0016 * (1 - t)) crackle = 0.25 + rnd.NextDouble() * 0.25;
            crackle *= 0.9;
            double pop = crackle * (rnd.NextDouble() * 2 - 1);
            s[i] = (float)Math.Tanh(clunk + buzz + pop);
        }
        return Fade(s, 0.002, 0.25);
    }

    // 20 s, a whole number of cycles of every tone, with its first 0.2 s crossfaded from the
    // samples just past the end, so it loops without a click.
    static float[] Hum() {
        var rnd = new Random(11);
        int n = Rate * 20, overlap = Rate / 5;
        var s = new float[n + overlap];
        double hiss = 0;
        for (int i = 0; i < s.Length; i++) {
            double t = i / (double)Rate;
            hiss += (rnd.NextDouble() * 2 - 1 - hiss) * 0.3;
            s[i] = (float)(Math.Sin(Tau * 15734 * t) * 0.009 + Math.Sin(Tau * 100 * t) * 0.012 + Math.Sin(Tau * 50 * t) * 0.008 + hiss * 0.004);
        }
        var loop = new float[n];
        for (int i = 0; i < n; i++) {
            double w = i < overlap ? i / (double)overlap : 1.0;
            loop[i] = (float)(s[i] * w + (i < overlap ? s[n + i] * (1 - w) : 0));
        }
        return loop;
    }

    static float[] PowerOff() {
        var rnd = new Random(3);
        int n = (int)(Rate * 0.9);
        var s = new float[n];
        double phase = 0, crackle = 0;
        for (int i = 0; i < n; i++) {
            double t = i / (double)Rate;
            // The picture collapsing: a falling zap.
            double f = 40 + 1400 * Math.Exp(-t * 11);
            phase += Tau * f / Rate;
            double zap = Math.Sin(phase) * Math.Exp(-t * 6) * 0.28;
            double thump = Math.Sin(Tau * 42 * t) * Math.Exp(-t * 9) * 0.4 * Smooth(0.0, 0.01, t);
            if (t < 0.3 && rnd.NextDouble() < 0.003) crackle = 0.3;
            crackle *= 0.88;
            s[i] = (float)Math.Tanh(zap + thump + crackle * (rnd.NextDouble() * 2 - 1));
        }
        return Fade(s, 0.001, 0.2);
    }

    static float[] Fade(float[] s, double inSeconds, double outSeconds) {
        int a = (int)(inSeconds * Rate), b = (int)(outSeconds * Rate);
        for (int i = 0; i < a && i < s.Length; i++) s[i] *= i / (float)a;
        for (int i = 0; i < b && i < s.Length; i++) s[s.Length - 1 - i] *= i / (float)b;
        return s;
    }
}
