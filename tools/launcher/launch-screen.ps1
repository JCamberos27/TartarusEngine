# The launch screen run-editor.cmd shows while the editor builds: the Tartarus banner, the art,
# then Genesis scrolling past at speed. The build runs in the background the whole time, so the
# show costs no extra launch time. Any key skips the animation.
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
try { [Console]::CursorVisible = $false } catch {}
$width = 120; $height = 30
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

# --- 1. banner: "Tartarus" over "Engine", a gold-to-ember gradient, then the binary --------------
$banner = $sections['banner']
$top = Center-Block ($banner | ForEach-Object { if ($_.Length -gt 96) { $_.Substring(0, 96) } else { $_ } } | Select-Object -First 9)
$bottom = Center-Block ($banner | ForEach-Object { if ($_.Length -gt 96) { $_.Substring(96) } else { '' } })
$rows = @($top) + @('') + @($bottom)

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
    $t = $i / [Math]::Max(1, $rows.Count - 1)
    $out.WriteLine("$(Rgb 255 ([int](215 - 110 * $t)) ([int](90 - 60 * $t)))$($rows[$i])$reset")
    Pause-Frame 30
}
$out.WriteLine(); $out.WriteLine()
foreach ($w in $binary) {
    $pad = ' ' * [Math]::Max(0, [int](($width - $w.Length) / 2))
    $out.WriteLine("$(Rgb 95 95 105)$pad$w$reset")
    Pause-Frame 30
}
Pause-Frame 900

# --- 2. the art, alone on screen, drawn in top to bottom and shaded by character density ----------
$art = @($sections['art'])
$fit = [int][Math]::Ceiling($art.Count / [double]($height - 2))
if ($fit -gt 1) { $art = @(Shrink-Art $art $fit) }
$shade = @{
    '@' = (Rgb 255 236 190); '%' = (Rgb 240 205 140); '#' = (Rgb 225 180 110)
    '*' = (Rgb 200 150 85);  '+' = (Rgb 170 120 70);  '=' = (Rgb 140 98 60)
    '-' = (Rgb 115 82 52);   ':' = (Rgb 95 70 48);    '.' = (Rgb 75 60 45)
}
$out.Write($clear + (Top-Pad $art.Count))
foreach ($l in (Center-Block $art)) {
    $sb = New-Object System.Text.StringBuilder
    $last = $null
    foreach ($ch in $l.ToCharArray()) {
        if ($ch -ne ' ') {
            $col = $shade[[string]$ch]; if (-not $col) { $col = $shade['@'] }
            if ($col -ne $last) { [void]$sb.Append($col); $last = $col }
        }
        [void]$sb.Append($ch)
    }
    $out.WriteLine($sb.Append($reset).ToString())
    Pause-Frame ([int](1200 / $art.Count))   # the whole figure draws in about a second
}
Pause-Frame 1800

# --- 3. Genesis at super speed ---------------------------------------------------------------------
$gold = Rgb 255 200 90; $verse = Rgb 210 150 70; $text = Rgb 170 170 178
$indent = ' ' * [Math]::Max(0, [int](($width - 76) / 2))   # the text is wrapped at 76 columns
$genesis = $sections['genesis']
$perFrame = 45   # ~5,700 lines at 45 per 8 ms frame: a couple of seconds end to end
$out.Write($clear + ("`n" * $height))
$sb = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $genesis.Count; $i++) {
    $l = $genesis[$i]
    if ($l -match '^Genesis Chapter') {
        [void]$sb.Append("`n$indent$gold$E[1m$l$E[22m$reset`n")
    } elseif ($l -match '^(\d+:\d+\.)(.*)$') {
        [void]$sb.Append("$indent$verse$($Matches[1])$text$($Matches[2])$reset`n")
    } else {
        [void]$sb.Append("$indent$text$l$reset`n")
    }
    if ((($i + 1) % $perFrame) -eq 0 -or $i -eq $genesis.Count - 1) {
        if (-not $skipped) { $out.Write($sb.ToString()) }
        [void]$sb.Clear()
        Pause-Frame 8
    }
}
$amen = 'Amen.'
$out.WriteLine("`n$(' ' * [Math]::Max(0, [int](($width - $amen.Length) / 2)))$gold$amen$reset`n")
try { [Console]::CursorVisible = $true } catch {}

# --- wait out the build ----------------------------------------------------------------------------
if ($buildProc) {
    $spin = '|/-\'; $n = 0
    while (-not $buildProc.HasExited) {
        $out.Write("`r  Building the latest (Release)... $($spin[$n % 4])")
        $n++
        Start-Sleep -Milliseconds 120
    }
    if ($n -gt 0) { $out.Write("`r" + (' ' * 44) + "`r") }
    $buildProc.WaitForExit()
    exit $buildProc.ExitCode
}
exit 0
