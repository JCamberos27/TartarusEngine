# The launch screen run-editor.cmd shows while the editor builds: the Tartarus banner, the art,
# then Genesis types rapidly into the console. The build runs in the background the whole time,
# so the show costs no extra launch time. Any key skips the animation.
#
#   launch-screen.ps1 [-Build]   -Build runs the Release build alongside and exits with its code
#
# The text lives in TartarusEngineAscii.txt, split into ::banner / ::binary / ::art / ::genesis.
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

# --- console helpers -------------------------------------------------------------------------------
# The desktop shortcut starts this as an ordinary (not maximized) window.  Give it a deliberate,
# generous size for the logo, then centre it in the usable desktop area.  The API calls
# are best-effort so launching from Windows Terminal or a redirected session still works normally.
function Set-LaunchConsoleLayout {
    param(
        [int]$Columns,
        [int]$Rows,
        [int]$FontHeight
    )
    $columns = $Columns; $rows = $Rows
    try {
        $columns = [Math]::Min($columns, [Console]::LargestWindowWidth)
        $rows = [Math]::Min($rows, [Console]::LargestWindowHeight)
        [Console]::SetBufferSize([Math]::Max([Console]::BufferWidth, $columns), [Math]::Max([Console]::BufferHeight, $rows))
        [Console]::SetWindowSize($columns, $rows)
    } catch {}

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
        [TartarusLaunchConsole]::UseFontHeight([int16]$FontHeight)
        # Recalculate after the smaller font is applied: it lets the complete supplied logo fit
        # without turning the launch window into a maximized full-screen console.
        try {
            $columns = [Math]::Min($columns, [Console]::LargestWindowWidth)
            $rows = [Math]::Min($rows, [Console]::LargestWindowHeight)
            [Console]::SetBufferSize([Math]::Max([Console]::BufferWidth, $columns), [Math]::Max([Console]::BufferHeight, $rows))
            [Console]::SetWindowSize($columns, $rows)
            # Match the scrollback buffer to the visible area. Genesis then scrolls naturally,
            # without reserving space for a persistent console scrollbar.
            [Console]::SetBufferSize($columns, $rows)
        } catch {}
        [TartarusLaunchConsole]::RemoveChromeAndScrollbar()
        [TartarusLaunchConsole]::Center()
    } catch {}
}

Set-LaunchConsoleLayout -Columns 166 -Rows 96 -FontHeight 10
try { [Console]::CursorVisible = $false; [Console]::ForegroundColor = [ConsoleColor]::White; [Console]::BackgroundColor = [ConsoleColor]::Black } catch {}
$width = 166; $height = 96
try { $width = [Math]::Max(40, [Console]::WindowWidth - 1); $height = [Math]::Max(20, [Console]::WindowHeight) } catch {}
$out = [Console]::Out
$skipped = $false

function Rgb([int]$r, [int]$g, [int]$b) { "$E[38;2;${r};${g};${b}m" }
$reset = "$E[0m"
$clear = "$E[2J$E[3J$E[H"

function Skip-Requested {
    if ($script:skipped) { return $true }
    try {
        if ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); $script:skipped = $true }
    } catch {}
    return $script:skipped
}

function Pause-Frame([int]$ms) { if (-not (Skip-Requested)) { Start-Sleep -Milliseconds $ms } }

# Centre a block of lines horizontally as one unit (keeps ASCII art aligned), cropped to the window.
function Center-Block([string[]]$lines) {
    $lead = [int]::MaxValue; $wide = 0
    foreach ($l in $lines) {
        if ($l.Trim().Length -eq 0) { continue }
        $lead = [Math]::Min($lead, $l.Length - $l.TrimStart().Length)
        $wide = [Math]::Max($wide, $l.Length)
    }
    if ($lead -eq [int]::MaxValue) { $lead = 0 }
    $pad = ' ' * [Math]::Max(0, [int](($width - ($wide - $lead)) / 2))
    foreach ($l in $lines) {
        $s = if ($l.Length -gt $lead) { $pad + $l.Substring($lead) } else { '' }
        if ($s.Length -gt $width) { $s = $s.Substring(0, $width) }
        $s
    }
}

# Blank lines that push a block of $rows lines to the vertical middle of the window.
function Top-Pad([int]$rows) { "`n" * [Math]::Max(0, [int](($height - $rows) / 2)) }

# Shrinks ASCII art by $f in both directions, keeping the densest character of each f x f cell.
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

# --- 1. banner: Tartarus over Engine, in clean white ------------------------------------------------
$banner = $sections['banner']
# Keep the whole supplied lockup visible on ordinary displays. Large ASCII titles are sampled down
# as a unit when necessary; this is preferable to hiding their right-hand side in a wider-than-screen
# console window.
$bannerWidth = ($banner | Measure-Object -Property Length -Maximum).Maximum
$bannerFit = [int][Math]::Ceiling($bannerWidth / [double][Math]::Max(1, $width - 4))
if ($bannerFit -gt 1) { $banner = @(Shrink-Art $banner $bannerFit) }
$rows = @(Center-Block $banner)

$binary = New-Object System.Collections.Generic.List[string]
foreach ($b in $sections['binary']) {
    $line = ''
    foreach ($w in ($b -split '\s+' | Where-Object { $_ })) {
        if ($line -and ($line.Length + $w.Length + 1) -gt ($width - 8)) { $binary.Add($line); $line = '' }
        $line = if ($line) { "$line $w" } else { $w }
    }
    if ($line) { $binary.Add($line) }
    $binary.Add('')
}

$out.Write($clear + (Top-Pad ($rows.Count + 2 + $binary.Count)))
for ($i = 0; $i -lt $rows.Count; $i++) {
    $out.WriteLine("$(Rgb 255 255 255)$($rows[$i])$reset")
    Pause-Frame 30
}
$out.WriteLine(); $out.WriteLine()
foreach ($w in $binary) {
    $pad = ' ' * [Math]::Max(0, [int](($width - $w.Length) / 2))
    $out.WriteLine("$(Rgb 255 255 255)$pad$w$reset")
    Pause-Frame 30
}
Pause-Frame 900

# --- 2. the art, alone on screen, drawn in top to bottom in white ----------------------------------
$art = @($sections['art'])
$artWidth = ($art | Measure-Object -Property Length -Maximum).Maximum
$fit = [int][Math]::Max(
    [Math]::Ceiling($art.Count / [double]($height - 2)),
    [Math]::Ceiling($artWidth / [double][Math]::Max(1, $width - 4)))
if ($fit -gt 1) { $art = @(Shrink-Art $art $fit) }
$white = Rgb 255 255 255
$out.Write($clear + (Top-Pad $art.Count))
foreach ($l in (Center-Block $art)) {
    $sb = New-Object System.Text.StringBuilder
    $last = $null
    foreach ($ch in $l.ToCharArray()) {
        if ($ch -ne ' ') {
            $col = $white
            if ($col -ne $last) { [void]$sb.Append($col); $last = $col }
        }
        [void]$sb.Append($ch)
    }
    $out.WriteLine($sb.Append($reset).ToString())
    Pause-Frame ([int](1200 / $art.Count))   # the whole figure draws in about a second
}
Pause-Frame 1800

# Collapse from the full-height artwork view into a tight reading frame. The intermediate steps
# preserve the window's centre and make the handoff feel deliberate rather than a hard snap.
$out.Write($clear)
foreach ($readingStep in @(
    @{ Columns = 104; Rows = 60 },
    @{ Columns =  96; Rows = 52 },
    @{ Columns =  88; Rows = 44 },
    @{ Columns =  80; Rows = 36 }
)) {
    Set-LaunchConsoleLayout -Columns $readingStep.Columns -Rows $readingStep.Rows -FontHeight 16
    Start-Sleep -Milliseconds 55
}
$width = 80; $height = 36
try { $width = [Math]::Max(40, [Console]::WindowWidth - 1); $height = [Math]::Max(20, [Console]::WindowHeight) } catch {}
$out = [Console]::Out

# --- 3. Genesis types at high speed ----------------------------------------------------------------
# SystemSounds is asynchronous. A tiny tick is played while text is arriving, throttled to a rate
# the Windows audio mixer can render instead of trying to queue thousands of overlapping sounds.
$gold = Rgb 255 200 90; $verse = Rgb 210 150 70; $text = Rgb 170 170 178
$indent = ' ' * [Math]::Max(0, [int](($width - 76) / 2))   # the text is wrapped at 76 columns
$genesis = $sections['genesis']
$charactersPerSecond = 150000
$charactersPerFrame = 256
$charactersPerWrite = 32
$soundEveryMilliseconds = 45
$out.Write($clear + ("`n" * $height))
$typed = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $genesis.Count; $i++) {
    $l = $genesis[$i]
    if ($l -match '^Genesis Chapter') {
        [void]$typed.Append("`n$indent$gold$E[1m$l$E[22m$reset`n")
    } elseif ($l -match '^(\d+:\d+\.)(.*)$') {
        [void]$typed.Append("$indent$verse$($Matches[1])$text$($Matches[2])$reset`n")
    } else {
        [void]$typed.Append("$indent$text$l$reset`n")
    }

}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$lastSoundAt = -$soundEveryMilliseconds
for ($i = 0; $i -lt $typed.Length; $i += $charactersPerWrite) {
    $count = [Math]::Min($charactersPerWrite, $typed.Length - $i)
    # Write short bursts rather than one host call per glyph. The result still visibly types in,
    # but it remains quick even with the full text of Genesis.
    $out.Write($typed.ToString($i, $count))
    if ((($i + $count) % $charactersPerFrame) -eq 0) {
        if (Skip-Requested) { break }
        $targetMilliseconds = (($i + $count) * 1000.0) / $charactersPerSecond
        if (($stopwatch.ElapsedMilliseconds - $lastSoundAt) -ge $soundEveryMilliseconds) {
            try { [System.Media.SystemSounds]::Beep.Play() } catch {}
            $lastSoundAt = $stopwatch.ElapsedMilliseconds
        }
        # A short spin preserves the fast, typewriter-like cadence without the 15 ms granularity
        # of Start-Sleep on many Windows hosts.
        while ($stopwatch.Elapsed.TotalMilliseconds -lt $targetMilliseconds) { [Threading.Thread]::SpinWait(128) }
    }
}
# --- keep the conclusion synchronized with the build -----------------------------------------------
$buildExitCode = 0
if ($buildProc) {
    $spin = '|/-\'; $n = 0
    while (-not $buildProc.HasExited) {
        $out.Write("`r  Building the latest (Release)... $($spin[$n % 4])")
        $n++
        Start-Sleep -Milliseconds 120
    }
    if ($n -gt 0) { $out.Write("`r" + (' ' * 44) + "`r") }
    $buildProc.WaitForExit()
    $buildExitCode = $buildProc.ExitCode
    if ($buildExitCode -ne 0) { exit $buildExitCode }
}

# Amen is deliberately withheld until the engine build has completed, so the final cue and the
# prompt always arrive together at the end of loading.
$amen = 'Amen.'
$promptPrefix = 'There is no '
$promptEsc = 'Esc'
$promptSuffix = 'ape.'
$prompt = "$promptPrefix$promptEsc$promptSuffix"
$promptRow = [Math]::Max(1, $height - 2)
$amenRow = [Math]::Max(1, $height - 1)
$amenPad = ' ' * [Math]::Max(0, [int](($width - $amen.Length) / 2))
$promptPad = ' ' * [Math]::Max(0, [int](($width - $prompt.Length) / 2))
$out.Write("$E[$amenRow;1H$E[2K$amenPad$gold$amen$reset")
$out.Write("$E[$promptRow;1H$E[2K$promptPad$text$promptPrefix$E[4m$promptEsc$E[24m$promptSuffix$reset")
try { [Console]::CursorVisible = $true } catch {}

# run-editor.cmd launches the editor only after this script exits. Waiting here turns the prompt
# into an intentional final transition instead of a message that flashes past. Escape is the only
# accepted key; every other key is ignored with a quiet system tick.
if ($Build) {
    try {
        do {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::Escape) {
                $accepted = Rgb 90 230 120
                $out.Write("$E[$promptRow;1H$E[2K$promptPad$text$promptPrefix$accepted$E[4m$promptEsc$E[24m$text$promptSuffix$reset")
                Start-Sleep -Milliseconds 180
            } else {
                [System.Media.SystemSounds]::Beep.Play()
            }
        } while ($key.Key -ne [ConsoleKey]::Escape)
    } catch {}
}
exit $buildExitCode
