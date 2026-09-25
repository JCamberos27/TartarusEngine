# The launch screen run-editor.cmd shows while the editor builds: the art fades in, the Tartarus
# lockup wipes in beneath it, and a status line tracks the build. The build runs in the background
# the whole time, so the show costs no extra launch time. Any key skips the animation.
#
#   launch-screen.ps1 [-Build]   -Build runs the Release build alongside and exits with its code
#
# The text lives in TartarusEngineAscii.txt, split into ::banner / ::art.
param([switch]$Build)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$E = [char]27

$sections = @{}
$current = $null
foreach ($line in [IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'TartarusEngineAscii.txt'))) {
    if ($line.StartsWith('::')) { $current = $line.Substring(2); $sections[$current] = New-Object System.Collections.Generic.List[string]; continue }
    if ($current) { $sections[$current].Add($line) }
}

# --- the build, started first so it overlaps the whole show -------------------------------------
$buildProc = $null
if ($Build) {
    $buildProc = Start-Process -FilePath 'cmd.exe' -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -ArgumentList '/c', 'cmake --build build --config Release --parallel > build\last-build.log 2>&1'
    $null = $buildProc.Handle # without this, Windows PowerShell never reports the ExitCode
}

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

# The composition, top to bottom: margin, art, gap, lockup, gap, status, margin.
$marginX = 4; $marginY = 2; $gapArt = 3; $gapStatus = 2
function Layout-Size {
    $script:cols = [Math]::Max((Block-Width $art), (Block-Width $banner)) + 2 * $marginX
    $script:rows = $marginY + $art.Count + $gapArt + $banner.Count + $gapStatus + 1 + $marginY
}
Layout-Size

# --- the window: exactly the composition's size, borderless, centred ------------------------------
# The API calls are best-effort so launching from Windows Terminal or a redirected session still
# works; the layout then crops to whatever window it gets.
function Set-LaunchConsoleLayout([int]$Columns, [int]$Rows) {
    try {
        if (-not ('TartarusLaunchConsole' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TartarusLaunchConsole {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X, Y; }
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
    public static int[] WorkArea() {
        RECT work; if (!SystemParametersInfo(48, 0, out work, 0)) return new int[] { 1920, 1040 };
        return new int[] { work.Right - work.Left, work.Bottom - work.Top };
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
    public static void Center() {
        IntPtr window = GetConsoleWindow(); if (window == IntPtr.Zero) return;
        RECT bounds, work; if (!GetWindowRect(window, out bounds) || !SystemParametersInfo(48, 0, out work, 0)) return;
        int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
        int x = work.Left + Math.Max(0, (work.Right - work.Left - width) / 2);
        int y = work.Top + Math.Max(0, (work.Bottom - work.Top - height) / 2);
        SetWindowPos(window, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010);
    }
}
'@ -ErrorAction Stop
        }
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
        [TartarusLaunchConsole]::Center()
    } catch {}
}

Set-LaunchConsoleLayout $cols $rows
try { [Console]::CursorVisible = $false; [Console]::ForegroundColor = [ConsoleColor]::White; [Console]::BackgroundColor = [ConsoleColor]::Black } catch {}
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
$artCol = [Math]::Max(0, [int](($winW - (Block-Width $art)) / 2)) + 1
$bannerCol = [Math]::Max(0, [int](($winW - (Block-Width $banner)) / 2)) + 1

$out = [Console]::Out
$skipped = $false
function Rgb([int]$r, [int]$g, [int]$b) { "$E[38;2;${r};${g};${b}m" }
function At([int]$row, [int]$col) { "$E[${row};${col}H" }
# Everything draws on true black (the console's own "black" is a grey 12,12,12).
$black = "$E[48;2;0;0;0m"
$reset = "$E[39m"

function Skip-Requested {
    if ($script:skipped) { return $true }
    try {
        if ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); $script:skipped = $true }
    } catch {}
    return $script:skipped
}
function Pause-Frame([int]$ms) { if (-not (Skip-Requested)) { Start-Sleep -Milliseconds $ms } }

# --- 1. the art fades up out of the dark -----------------------------------------------------------
# Each glyph is lit by its density, so the figure keeps its depth instead of reading flat white.
$tone = @{ '.' = 0.34; ':' = 0.46; '-' = 0.56; '=' = 0.66; '+' = 0.74; '*' = 0.82; '#' = 0.90; '%' = 0.96; '@' = 1.0 }
function Draw-Art([double]$level) {
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $art.Count; $i++) {
        $line = $art[$i]; if (-not $line) { continue }
        [void]$sb.Append((At ($artRow + $i) $artCol))
        $last = -1
        foreach ($ch in $line.ToCharArray()) {
            if ($ch -ne ' ') {
                $t = $tone["$ch"]; if ($null -eq $t) { $t = 1.0 }
                $v = [int](255 * $level * $t)
                if ($v -ne $last) { [void]$sb.Append((Rgb $v $v ([Math]::Min(255, $v + 6)))); $last = $v }
            }
            [void]$sb.Append($ch)
        }
    }
    $out.Write($sb.Append($reset).ToString())
}

# --- 2. the lockup wipes in, left to right, with a warm leading edge --------------------------------
function Draw-Banner([int]$upTo) {
    $sb = New-Object System.Text.StringBuilder
    $edge = 10
    for ($i = 0; $i -lt $banner.Count; $i++) {
        $line = $banner[$i]; if (-not $line) { continue }
        $n = [Math]::Min($upTo, $line.Length); if ($n -le 0) { continue }
        $solid = [Math]::Max(0, [Math]::Min($n, $upTo - $edge))
        [void]$sb.Append((At ($bannerRow + $i) $bannerCol))
        [void]$sb.Append((Rgb 236 236 240)).Append($line.Substring(0, $solid))
        [void]$sb.Append((Rgb 255 214 140)).Append($line.Substring($solid, $n - $solid))
    }
    $out.Write($sb.Append($reset).ToString())
}

# --- 3. the status line, letter-spaced under the lockup -------------------------------------------
function Draw-Status([string]$text, [string]$color) {
    $spaced = ($text.ToCharArray() -join ' ')
    $col = [Math]::Max(1, [int](($winW - $spaced.Length) / 2) + 1)
    $out.Write("$(At $statusRow 1)$E[2K$(At $statusRow $col)$color$spaced$reset")
}

$out.Write("$black$E[2J$E[3J$E[H")
foreach ($level in 0.06, 0.14, 0.26, 0.4, 0.56, 0.72, 0.86, 1.0) {
    if (Skip-Requested) { break }
    Draw-Art $level
    Start-Sleep -Milliseconds 90
}
Draw-Art 1.0
Pause-Frame 500

$bannerWidth = Block-Width $banner
for ($x = 8; $x -lt $bannerWidth + 10; $x += 8) {
    if (Skip-Requested) { break }
    Draw-Banner $x
    Start-Sleep -Milliseconds 10
}
Draw-Banner ($bannerWidth + 10)
Pause-Frame 400

# --- the build: a quiet pulse until it finishes, then the editor starts ---------------------------
$dim = Rgb 120 120 128
$buildExitCode = 0
if ($buildProc) {
    $n = 0
    while (-not $buildProc.HasExited) {
        $dots = ('.' * ($n % 4)).PadRight(3)
        Draw-Status "BUILDING$dots" $dim
        $n++
        Start-Sleep -Milliseconds 350
    }
    $buildProc.WaitForExit()
    $buildExitCode = $buildProc.ExitCode
}
if ($buildExitCode -ne 0) {
    Draw-Status 'BUILD FAILED' (Rgb 235 90 80)
    Start-Sleep -Milliseconds 1200
} else {
    Draw-Status 'READY' (Rgb 120 220 140)
    Start-Sleep -Milliseconds 900
}
$out.Write("$E[0m$E[2J$E[H")
try { [Console]::CursorVisible = $true } catch {}
exit $buildExitCode
