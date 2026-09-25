# The launch screen run-editor.cmd shows while the editor builds: the art fades in, the Tartarus
# lockup wipes in beneath it, and a status line and progress bar track the build. The build runs
# in the background the whole time, so the show costs no extra launch time. Any key skips the
# animation.
#
#   launch-screen.ps1 [-Build] [-Reveal]
#     -Build   runs the Release build alongside. Exit codes: 0 built; on a failed build the screen
#              lists the errors and asks - 10 launch the previous build, 11 close.
#     -Reveal  only shows the console window (run-editor.cmd starts it minimized) and exits.
#
# The text lives in TartarusEngineAscii.txt, split into ::banner / ::art.
param([switch]$Build, [switch]$Reveal)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$E = [char]27

# --- the console window API -------------------------------------------------------------------------
function Load-ConsoleApi {
    if ('TartarusLaunchConsole' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TartarusLaunchConsole {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct MONITORINFO { public int Size; public RECT Monitor, Work; public uint Flags; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct CONSOLE_FONT_INFOEX {
        public uint Size; public uint Font; public COORD FontSize; public int Family, Weight;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FaceName;
    }
    [DllImport("kernel32.dll")] static extern IntPtr GetConsoleWindow();
    [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool SetCurrentConsoleFontEx(IntPtr output, bool maximumWindow, ref CONSOLE_FONT_INFOEX info);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(int action, int param, out RECT rect, int flags);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")] static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtr")] static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int index, IntPtr value);
    [DllImport("user32.dll")] static extern bool ShowScrollBar(IntPtr hWnd, int bar, bool show);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadImage(IntPtr instance, string name, uint type, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    // The work area of the monitor under the mouse: the screen is shown where the user is looking.
    static bool Work(out RECT work) {
        POINT p; MONITORINFO info = new MONITORINFO(); info.Size = Marshal.SizeOf(typeof(MONITORINFO));
        if (GetCursorPos(out p) && GetMonitorInfo(MonitorFromPoint(p, 1), ref info)) { work = info.Work; return true; }
        return SystemParametersInfo(48, 0, out work, 0);
    }
    public static int[] WorkArea() {
        RECT work; if (!Work(out work)) return new int[] { 1920, 1040 };
        return new int[] { work.Right - work.Left, work.Bottom - work.Top };
    }
    // The engine's icon on the window and its taskbar button, in place of the console's.
    public static void SetIcon(string path) {
        IntPtr window = GetConsoleWindow(); if (window == IntPtr.Zero) return;
        IntPtr big = LoadImage(IntPtr.Zero, path, 1, 32, 32, 0x10), small = LoadImage(IntPtr.Zero, path, 1, 16, 16, 0x10);
        if (big != IntPtr.Zero) SendMessage(window, 0x0080, (IntPtr)1, big);    // WM_SETICON, ICON_BIG
        if (small != IntPtr.Zero) SendMessage(window, 0x0080, IntPtr.Zero, small);
    }
    [ComImport, Guid("ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface ITaskbarList3 {
        void HrInit(); void AddTab(IntPtr h); void DeleteTab(IntPtr h); void ActivateTab(IntPtr h); void SetActiveAlt(IntPtr h);
        void MarkFullscreenWindow(IntPtr h, int fullscreen);
        void SetProgressValue(IntPtr h, ulong completed, ulong total);
        void SetProgressState(IntPtr h, int state);
    }
    [ComImport, Guid("56FDF344-FD6D-11d0-958A-006097C9A090"), ClassInterface(ClassInterfaceType.None)] class TaskbarList { }
    static ITaskbarList3 taskbar;
    // The build's progress on the taskbar button too. States: 0 none, 2 normal, 4 error.
    public static void TaskbarProgress(int state, double fraction) {
        try {
            IntPtr window = GetConsoleWindow(); if (window == IntPtr.Zero) return;
            if (taskbar == null) { taskbar = (ITaskbarList3)new TaskbarList(); taskbar.HrInit(); }
            taskbar.SetProgressState(window, state);
            if (state != 0) taskbar.SetProgressValue(window, (ulong)(Math.Max(0.0, Math.Min(1.0, fraction)) * 1000), 1000);
        } catch { }
    }
    public static void UseFontHeight(short height) {
        CONSOLE_FONT_INFOEX info = new CONSOLE_FONT_INFOEX();
        info.Size = (uint)Marshal.SizeOf(typeof(CONSOLE_FONT_INFOEX)); info.FontSize.Y = height;
        info.Family = 54; info.Weight = 400; info.FaceName = "Consolas";
        SetCurrentConsoleFontEx(GetStdHandle(-11), false, ref info);
    }
    public static void RemoveChromeAndScrollbar() {
        IntPtr window = GetConsoleWindow(); if (window == IntPtr.Zero) return;
        const long chromeAndScrollbars = 0x00C00000L | 0x00040000L | 0x00100000L | 0x00200000L;
        long style = GetWindowLongPtr(window, -16).ToInt64();
        SetWindowLongPtr(window, -16, new IntPtr(style & ~chromeAndScrollbars));
        ShowScrollBar(window, 1, false); // SB_VERT
        ShowScrollBar(window, 0, false); // SB_HORZ
        SetWindowPos(window, IntPtr.Zero, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020);
    }
    [DllImport("user32.dll")] static extern bool SetLayeredWindowAttributes(IntPtr hWnd, uint key, byte alpha, uint flags);
    // run-editor.cmd starts the console minimized, so its default-sized window is never seen. The
    // console can't be sized while minimized, so restore it fully transparent first.
    public static void RestoreHidden() {
        IntPtr window = GetConsoleWindow(); if (window == IntPtr.Zero || !IsIconic(window)) return;
        long ex = GetWindowLongPtr(window, -20).ToInt64();
        SetWindowLongPtr(window, -20, new IntPtr(ex | 0x00080000L)); // WS_EX_LAYERED
        SetLayeredWindowAttributes(window, 0, 0, 2);                   // LWA_ALPHA, fully clear
        ShowWindow(window, 9);                                          // SW_RESTORE
    }
    // Centres the window, then makes it opaque and brings it forward.
    public static void ShowCentered() {
        IntPtr window = GetConsoleWindow(); if (window == IntPtr.Zero) return;
        RECT work, bounds;
        if (Work(out work) && GetWindowRect(window, out bounds)) {
            int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
            int x = work.Left + Math.Max(0, (work.Right - work.Left - width) / 2);
            int y = work.Top + Math.Max(0, (work.Bottom - work.Top - height) / 2);
            SetWindowPos(window, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010);
        }
        if ((GetWindowLongPtr(window, -20).ToInt64() & 0x00080000L) != 0) SetLayeredWindowAttributes(window, 0, 255, 2);
        SetForegroundWindow(window);
    }
    public static void Reveal() {
        IntPtr window = GetConsoleWindow(); if (window != IntPtr.Zero && IsIconic(window)) ShowWindow(window, 9);
    }
}
'@ -ErrorAction Stop
}

if ($Reveal) {
    try { Load-ConsoleApi; [TartarusLaunchConsole]::Reveal() } catch {}
    exit 0
}

$sections = @{}
$current = $null
foreach ($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'TartarusEngineAscii.txt'))) {
    if ($line.StartsWith('::')) { $current = $line.Substring(2); $sections[$current] = New-Object System.Collections.Generic.List[string]; continue }
    if ($current) { $sections[$current].Add($line) }
}

# --- the build, started first so it overlaps the whole show -------------------------------------
$buildLog = Join-Path $root 'build\last-build.log'
$progressFile = Join-Path $root 'build\launch-progress.txt'
$buildProc = $null
if ($Build) {
    $buildProc = Start-Process -FilePath 'cmd.exe' -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -ArgumentList '/c', 'cmake --build build --config Release --parallel > build\last-build.log 2>&1'
    $null = $buildProc.Handle # without this, Windows PowerShell never reports the ExitCode
}
$clock = [Diagnostics.Stopwatch]::StartNew()

# The version line: the project version, and the branch and commit being built.
$versionText = ''
try {
    $cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
    if ($cmake -match 'project\(\s*\w+\s+VERSION\s+([\d.]+)') { $versionText = "v$($Matches[1])" }
} catch {}
try {
    $branch = (& git -C $root rev-parse --abbrev-ref HEAD 2>$null | Select-Object -First 1)
    $commit = (& git -C $root rev-parse --short HEAD 2>$null | Select-Object -First 1)
    if ($branch -and $commit) { $versionText = (@($versionText, "$branch @ $commit") | Where-Object { $_ }) -join "   $([char]0x00B7)   " }
} catch {}

# --- the blocks ------------------------------------------------------------------------------------
# Each block loses its blank rows and its common indent, so the layout sizes to the ink alone.
function Trim-Block([string[]]$lines) {
    $lines = @($lines | ForEach-Object { $_.TrimEnd() })
    $first = 0; while ($first -lt $lines.Count -and -not $lines[$first]) { $first++ }
    $last = $lines.Count - 1; while ($last -ge $first -and -not $lines[$last]) { $last-- }
    if ($first -gt $last) { return @() }
    $lines = $lines[$first..$last]
    $lead = ($lines | Where-Object { $_ } | ForEach-Object { $_.Length - $_.TrimStart().Length } | Measure-Object -Minimum).Minimum
    @($lines | ForEach-Object { if ($_.Length -gt $lead) { $_.Substring($lead) } else { '' } })
}

# Shrinks ASCII art by $f in both directions, keeping the densest character of each f x f cell.
# Only used when the display can't fit the full-size layout.
function Shrink-Art([string[]]$lines, [int]$f) {
    $ramp = ' .:-=+*#%@'
    $w = ($lines | Measure-Object -Property Length -Maximum).Maximum
    for ($y = 0; $y -lt $lines.Count; $y += $f) {
        $sb = New-Object System.Text.StringBuilder
        for ($x = 0; $x -lt $w; $x += $f) {
            $best = ' '; $rank = 0
            for ($dy = 0; $dy -lt $f -and ($y + $dy) -lt $lines.Count; $dy++) {
                $row = $lines[$y + $dy]
                for ($dx = 0; $dx -lt $f -and ($x + $dx) -lt $row.Length; $dx++) {
                    $c = $row[$x + $dx]; $r = $ramp.IndexOf($c)
                    if ($r -lt 0 -and $c -ne ' ') { $r = 9 }
                    if ($r -gt $rank) { $rank = $r; $best = $c }
                }
            }
            [void]$sb.Append($best)
        }
        $sb.ToString().TrimEnd()
    }
}

function Block-Width([string[]]$lines) { [int]($lines | Measure-Object -Property Length -Maximum).Maximum }

$art = @(Trim-Block $sections['art'])
$banner = @(Trim-Block $sections['banner'])

# The composition, top to bottom: margin, art, gap, lockup, gap, status, gap, progress bar, gap,
# version, margin.
$marginX = 4; $marginY = 2; $gapArt = 3; $gapStatus = 2; $gapBar = 1; $gapVersion = 2
$barWidth = 64
function Layout-Size {
    $script:cols = [Math]::Max((Block-Width $art), (Block-Width $banner)) + 2 * $marginX
    $script:rows = $marginY + $art.Count + $gapArt + $banner.Count + $gapStatus + 1 + $gapBar + 1 + $gapVersion + 1 + $marginY
}
Layout-Size

# --- the window: exactly the composition's size, borderless, centred ------------------------------
# The API calls are best-effort so launching from Windows Terminal or a redirected session still
# works; the layout then crops to whatever window it gets.
function Set-LaunchConsoleLayout([int]$Columns, [int]$Rows) {
    try {
        Load-ConsoleApi
        [TartarusLaunchConsole]::RestoreHidden()
        # The largest Consolas size (a cell is about h tall, h/2 wide) whose window fills no more
        # than ~90% of the desktop.
        $work = [TartarusLaunchConsole]::WorkArea()
        $font = [int][Math]::Floor([Math]::Min($work[1] * 0.9 / $Rows, $work[0] * 0.9 / ($Columns * 0.55)))
        [TartarusLaunchConsole]::UseFontHeight([int16][Math]::Max(6, [Math]::Min(16, $font)))
        $c = [Math]::Min($Columns, [Console]::LargestWindowWidth)
        $r = [Math]::Min($Rows, [Console]::LargestWindowHeight)
        # Grow the buffer first (a window can't outsize it), then match it to the window exactly.
        [Console]::SetBufferSize([Math]::Max([Console]::BufferWidth, $c), [Math]::Max([Console]::BufferHeight, $r))
        [Console]::SetWindowSize($c, $r)
        [Console]::SetBufferSize($c, $r)
        [TartarusLaunchConsole]::RemoveChromeAndScrollbar()
    } catch {}
}

try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}
$out = [Console]::Out
# Everything draws on true black (the console's own "black" is a grey 12,12,12). The screen is
# blacked out before the window is shown, so the first thing seen is the empty stage.
$black = "$E[48;2;0;0;0m"
try { [Console]::CursorVisible = $false } catch {}
$out.Write("$black$E[2J$E[3J$E[H")
Set-LaunchConsoleLayout $cols $rows
$out.Write("$black$E[2J$E[3J$E[H")
try { [Console]::Title = 'Tartarus Engine' } catch {}
try { [TartarusLaunchConsole]::SetIcon((Join-Path $root 'extern\branding\tartarus_icon.ico')) } catch {}
try { [TartarusLaunchConsole]::ShowCentered() } catch {}

$winW = $cols; $winH = $rows
try { $winW = [Console]::WindowWidth; $winH = [Console]::WindowHeight } catch {}
# A display too small for the full size: halve the art rather than crop it.
if ($winW -lt $cols -or $winH -lt $rows) {
    $art = @(Shrink-Art $art 2)
    if ($winW -lt (Block-Width $banner) + 2) { $banner = @(Shrink-Art $banner 2) }
    Layout-Size
}
# Centre the composition in whatever window we have (exactly the window, normally).
$top = [Math]::Max(0, [int](($winH - $rows) / 2)) + $marginY
$artRow = $top + 1                                        # rows are 1-based in VT sequences
$bannerRow = $artRow + $art.Count + $gapArt
$statusRow = $bannerRow + $banner.Count + $gapStatus
$barRow = $statusRow + 1 + $gapBar
$versionRow = $barRow + 1 + $gapVersion
$artWidth = Block-Width $art
$artCol = [Math]::Max(0, [int](($winW - $artWidth) / 2)) + 1
$bannerWidth = Block-Width $banner
$bannerCol = [Math]::Max(0, [int](($winW - $bannerWidth) / 2)) + 1
$barWidth = [Math]::Min($barWidth, $winW - 2 * $marginX)
$barCol = [Math]::Max(0, [int](($winW - $barWidth) / 2)) + 1

$skipped = $false
function Rgb([double]$r, [double]$g, [double]$b) {
    $r = [int][Math]::Max(0.0, [Math]::Min(255.0, $r)); $g = [int][Math]::Max(0.0, [Math]::Min(255.0, $g)); $b = [int][Math]::Max(0.0, [Math]::Min(255.0, $b))
    "$E[38;2;${r};${g};${b}m"
}
function At([int]$row, [int]$col) { "$E[${row};${col}H" }

function Skip-Requested {
    if ($script:skipped) { return $true }
    try {
        if ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); $script:skipped = $true }
    } catch {}
    return $script:skipped
}
function Pause-Frame([int]$ms) { if (-not (Skip-Requested)) { Start-Sleep -Milliseconds $ms } }

# --- the art -----------------------------------------------------------------------------------------
# Each glyph is lit by its density, so the figure keeps its depth instead of reading flat white.
# The lines are split once into runs of one tone, so a redraw at a new brightness is cheap.
$tone = @{ '.' = 0.34; ':' = 0.46; '-' = 0.56; '=' = 0.66; '+' = 0.74; '*' = 0.82; '#' = 0.90; '%' = 0.96; '@' = 1.0 }
$artRuns = @(foreach ($line in $art) {
    $runs = New-Object System.Collections.Generic.List[object]
    $i = 0
    while ($i -lt $line.Length) {
        $ch = [string]$line[$i]
        $t = if ($ch -eq ' ') { -1.0 } elseif ($tone.ContainsKey($ch)) { $tone[$ch] } else { 1.0 }
        $j = $i + 1
        while ($j -lt $line.Length) {
            $c = [string]$line[$j]
            $u = if ($c -eq ' ') { -1.0 } elseif ($tone.ContainsKey($c)) { $tone[$c] } else { 1.0 }
            if ($u -ne $t) { break }
            $j++
        }
        $runs.Add(@($t, $line.Substring($i, $j - $i)))
        $i = $j
    }
    , $runs
})
function Draw-Art([double]$level) {
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $artRuns.Count; $i++) {
        $runs = $artRuns[$i]; if ($runs.Count -eq 0) { continue }
        [void]$sb.Append((At ($artRow + $i) $artCol))
        foreach ($r in $runs) {
            if ($r[0] -ge 0) { $v = 255 * $level * $r[0]; [void]$sb.Append((Rgb $v $v ($v + 6))) }
            [void]$sb.Append($r[1])
        }
    }
    $out.Write($sb.ToString())
}

# --- the lockup ----------------------------------------------------------------------------------
# Drawn up to column $upTo (the wipe), with a gold leading edge while it travels. $warm tints the
# whole lockup toward candlelight, $level dims it.
function Draw-Banner([int]$upTo, [double]$level = 1.0, [double]$warm = 0.0) {
    $sb = New-Object System.Text.StringBuilder
    $edge = 10
    $body = Rgb ((236 + 19 * $warm) * $level) ((236 - 26 * $warm) * $level) ((240 - 100 * $warm) * $level)
    $gold = Rgb (255 * $level) (214 * $level) (140 * $level)
    for ($i = 0; $i -lt $banner.Count; $i++) {
        $line = $banner[$i]; if (-not $line) { continue }
        $n = [Math]::Min($upTo, $line.Length); if ($n -le 0) { continue }
        $solid = [Math]::Max(0, [Math]::Min($n, $upTo - $edge))
        [void]$sb.Append((At ($bannerRow + $i) $bannerCol))
        [void]$sb.Append($body).Append($line.Substring(0, $solid))
        [void]$sb.Append($gold).Append($line.Substring($solid, $n - $solid))
    }
    $out.Write($sb.ToString())
}

# --- the status, bar and version lines -------------------------------------------------------------
function Draw-Centered([int]$row, [string]$text, [string]$color) {
    $col = [Math]::Max(1, [int](($winW - $text.Length) / 2) + 1)
    $out.Write("$(At $row 1)$E[2K$(At $row $col)$color$text")
}
function Draw-Status([string]$text, [string]$color) { Draw-Centered $statusRow ($text.ToCharArray() -join ' ') $color }
function Draw-Bar([double]$fraction, [double]$level = 1.0) {
    $filled = [int][Math]::Round([Math]::Max(0.0, [Math]::Min(1.0, $fraction)) * $barWidth)
    $out.Write("$(At $barRow $barCol)$(Rgb (228 * $level) (184 * $level) (104 * $level))$([string][char]0x2501 * $filled)" +
               "$(Rgb (44 * $level) (44 * $level) (50 * $level))$([string][char]0x2500 * ($barWidth - $filled))")
}
function Draw-Version([double]$level = 1.0) {
    if ($versionText) { Draw-Centered $versionRow $versionText (Rgb (78 * $level) (78 * $level) (86 * $level)) }
}

# --- 1. the art fades up out of the dark -----------------------------------------------------------
foreach ($level in 0.06, 0.14, 0.26, 0.4, 0.56, 0.72, 0.86, 1.0) {
    if (Skip-Requested) { break }
    Draw-Art $level
    Start-Sleep -Milliseconds 60
}
Draw-Art 1.0
Pause-Frame 450

# --- 2. the lockup wipes in, left to right ---------------------------------------------------------
for ($x = 8; $x -lt $bannerWidth + 10; $x += 8) {
    if (Skip-Requested) { break }
    Draw-Banner $x
    Start-Sleep -Milliseconds 10
}
Draw-Banner ($bannerWidth + 10)
Draw-Version
Pause-Frame 250

# --- 3. the build: a pulse, a progress bar and a candlelit lockup until it finishes ----------------
# MSBuild prints one "Project.vcxproj -> output" line per finished project; progress counts them
# against the last successful build, with its duration filling in the long compiles between.
$expectProjects = 0; $expectSeconds = 0.0
try {
    $p = ([IO.File]::ReadAllText($progressFile)).Trim() -split '\s+'
    $expectProjects = [int]$p[0]; $expectSeconds = [double]::Parse($p[1], [Globalization.CultureInfo]::InvariantCulture)
} catch {}
function Read-BuildLog {
    try {
        $fs = New-Object IO.FileStream($buildLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
        try { return (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}
function Count-Projects([string]$log) { ([regex]::Matches($log, '\.vcxproj -> ')).Count }

$dim = Rgb 120 120 128
$buildExitCode = 0
$shown = 0.0; $projects = 0
if ($buildProc) {
    $tick = 0; $flicker = 0.12; $flickerTarget = 0.12
    $rand = New-Object Random
    while (-not $buildProc.HasExited) {
        if ($tick % 4 -eq 0) { $projects = Count-Projects (Read-BuildLog) }
        $t = $clock.Elapsed.TotalSeconds
        $byProjects = if ($expectProjects -gt 0) { $projects / [double]$expectProjects } else { 0.0 }
        $byTime = if ($expectSeconds -gt 0) { 0.9 * $t / $expectSeconds } else { 0.9 * (1 - [Math]::Exp(-$t / 90)) }
        $target = [Math]::Min(0.97, [Math]::Max($byProjects, $byTime))
        if ($target -gt $shown) { $shown += ($target - $shown) * 0.25 }

        # Candlelight: the lockup's warmth wanders, eased, toward a new random level.
        if ($tick % 5 -eq 0) { $flickerTarget = 0.05 + $rand.NextDouble() * 0.35 }
        $flicker += ($flickerTarget - $flicker) * 0.3
        Draw-Banner ($bannerWidth + 10) (1.0 - $flicker * 0.18) $flicker
        if ($tick % 5 -eq 0) { Draw-Status ('BUILDING' + ('.' * (($tick / 5) % 4)).PadRight(3)) $dim }
        Draw-Bar $shown
        [TartarusLaunchConsole]::TaskbarProgress(2, $shown)
        $tick++
        Start-Sleep -Milliseconds 70
    }
    $buildProc.WaitForExit()
    $buildExitCode = $buildProc.ExitCode
    $buildSeconds = 0.0
    try { $buildSeconds = ($buildProc.ExitTime - $buildProc.StartTime).TotalSeconds } catch {}
    Draw-Banner ($bannerWidth + 10)
}

# --- a failed build: the errors, on screen, and a choice -------------------------------------------
if ($buildExitCode -ne 0) {
    try { [TartarusLaunchConsole]::TaskbarProgress(4, 1.0) } catch {}
    foreach ($level in 0.7, 0.45, 0.25, 0.1, 0.0) { Draw-Art $level; Start-Sleep -Milliseconds 40 }
    $log = Read-BuildLog
    $prefix = [string]$root + '\'
    $errors = New-Object System.Collections.Generic.List[string]
    foreach ($l in ($log -split "`r?`n")) {
        if ($l -notmatch '(?i)(\berror\b|CMake Error)') { continue }
        $l = ($l -replace '\s*\[[^\]]*\.vcxproj\]\s*$', '').Trim()
        $l = $l.Replace($prefix, '').Replace($prefix.Replace('\', '/'), '')
        if (-not $errors.Contains($l)) { $errors.Add($l) }
    }
    if ($errors.Count -eq 0) {
        foreach ($l in (($log -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -Last 8)) { $errors.Add($l.Trim()) }
    }
    $maxLines = [Math]::Min(14, [Math]::Max(1, $art.Count - 12))
    $shownErrors = @($errors | Select-Object -First $maxLines)
    $textWidth = $winW - 2 * $marginX - 4
    $row = $artRow + [Math]::Max(0, [int](($art.Count - $shownErrors.Count - 6) / 2))
    Draw-Centered $row ('BUILD FAILED'.ToCharArray() -join ' ') (Rgb 235 90 80)
    $row += 3
    $blockWidth = [Math]::Min($textWidth, (($shownErrors | Measure-Object -Property Length -Maximum).Maximum))
    $col = [Math]::Max(1, [int](($winW - $blockWidth) / 2) + 1)
    foreach ($l in $shownErrors) {
        if ($l.Length -gt $textWidth) { $l = $l.Substring(0, $textWidth - 3) + '...' }
        $out.Write("$(At $row $col)$(Rgb 236 128 116)$l"); $row++
    }
    if ($errors.Count -gt $shownErrors.Count) {
        $out.Write("$(At $row $col)$(Rgb 150 90 86)... and $($errors.Count - $shownErrors.Count) more in build\last-build.log"); $row++
    }
    Draw-Centered ($row + 2) 'The previous build does not contain your latest changes.' (Rgb 110 110 118)

    Draw-Status 'BUILD FAILED' (Rgb 235 90 80)
    $out.Write("$(At $barRow 1)$E[2K")
    Draw-Centered $barRow '[Y]  launch the previous build        [N]  close' (Rgb 200 200 206)
    try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch {}
    $choice = 11
    try {
        while ($true) {
            $key = [Console]::ReadKey($true).Key
            if ($key -eq [ConsoleKey]::Y) { $choice = 10; break }
            if ($key -eq [ConsoleKey]::N -or $key -eq [ConsoleKey]::Escape) { break }
        }
    } catch {}
    $out.Write("$E[0m$E[2J$E[H")
    try { [Console]::CursorVisible = $true } catch {}
    exit $choice
}

# --- done: fill the bar, READY, and fade the whole screen to black ---------------------------------
if ($buildProc) {
    while ($shown -lt 0.995) {
        $shown += (1 - $shown) * 0.45; Draw-Bar $shown
        try { [TartarusLaunchConsole]::TaskbarProgress(2, $shown) } catch {}
        Start-Sleep -Milliseconds 25
    }
    Draw-Bar 1.0
    # Next launch's progress estimate. A build that was only up to date says little; keep the old.
    $projects = Count-Projects (Read-BuildLog)
    if ($projects -gt 0 -and ($clock.Elapsed.TotalSeconds -gt 8 -or $expectProjects -eq 0)) {
        try {
            [IO.File]::WriteAllText($progressFile, "$projects $($clock.Elapsed.TotalSeconds.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture))")
        } catch {}
    }
}
# READY, and how long the build took.
$builtIn = ''
if ($buildProc -and $buildSeconds -gt 0) {
    $builtIn = if ($buildSeconds -ge 60) { '{0} min {1} s' -f [int][Math]::Floor($buildSeconds / 60), [int]($buildSeconds % 60) }
               else { $buildSeconds.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture) + ' s' }
    $builtIn = "built in $builtIn"
}
function Draw-Ready([double]$level = 1.0) {
    $word = 'READY'.ToCharArray() -join ' '
    $tail = if ($builtIn) { "     $builtIn" } else { '' }
    $col = [Math]::Max(1, [int](($winW - $word.Length - $tail.Length) / 2) + 1)
    $out.Write("$(At $statusRow 1)$E[2K$(At $statusRow $col)$(Rgb (120 * $level) (220 * $level) (140 * $level))$word" +
               "$(Rgb (96 * $level) (96 * $level) (104 * $level))$tail")
}
Draw-Ready
try { [TartarusLaunchConsole]::TaskbarProgress(0, 0) } catch {}
Start-Sleep -Milliseconds 750
foreach ($level in 0.8, 0.6, 0.42, 0.26, 0.13, 0.04, 0.0) {
    Draw-Art $level
    Draw-Banner ($bannerWidth + 10) $level
    Draw-Ready $level
    if ($buildProc) { Draw-Bar 1.0 $level }
    Draw-Version $level
    Start-Sleep -Milliseconds 45
}
$out.Write("$E[0m$E[2J$E[H")
try { [Console]::CursorVisible = $true } catch {}
exit 0
