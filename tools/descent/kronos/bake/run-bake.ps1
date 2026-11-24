# W5 GI Phase 1: one --bake-gi of Forest in the wt-gpu runner. Hidden, BelowNormal.
# Caller holds locks\perf. -Tag names the outputs (e.g. pinned-lr2-a). -Only passes --gi-only. -Doors passes --bake-gi-doors (start|open|shut|hidden). -Live sets
# TARTARUS_GI_BAKE_LIVE_CLOCKS=1 (the old, unpinned clocks) for the A/B.
# Writes <Tag>.{stdout,stderr,summary}.txt and Forest.<Tag>.gi here, then puts wt-gpu's tracked Forest.gi back.
# wt-gpu\project\scenes\Forest.gi was checked to be a regular file (one link) on 2026-11-23. Never touches payloads.
param([Parameter(Mandatory)][string]$Tag, [string]$Only = '', [string]$Doors = '', [switch]$Live, [int]$TimeoutMin = 45)
$ErrorActionPreference = 'Stop'
if (Test-Path (Join-Path $PSScriptRoot 'STOP')) { "STOP file present: not running"; exit 7 }
$g = 'D:\Jacob\tblocks\descent\wt-gpu'
$k = 'D:\Jacob\tblocks\descent\gi-p1\kronos'
$gi = "$g\project\scenes\Forest.gi"
if (git -C $g status --porcelain -- project/scenes/Forest.gi) { "Forest.gi already modified before the run: STOP" | Out-File "$k\$Tag.summary.txt"; exit 5 }
$free = (Get-PSDrive D).Free / 1GB
if ($free -lt 100) { "D: free {0:N2} GB < 100 GB team floor: not running" -f $free | Out-File "$k\$Tag.summary.txt"; exit 3 }
# Never run while Jacob's editor (or any engine outside this runner) is up. Look only; never touch its pid.
$other = @(Get-Process TartarusEngine -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path -notlike "$g\*" })
if ($other.Count) { "STOP: another TartarusEngine is running (pid $($other.Id -join ',')): not running" | Out-File "$k\$Tag.summary.txt"; exit 6 }
$links = @(fsutil hardlink list $gi)
if ($links.Count -ne 1) { "Forest.gi has $($links.Count) links: STOP" | Out-File "$k\$Tag.summary.txt"; exit 4 }
$env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
if ($Live) { $env:TARTARUS_GI_BAKE_LIVE_CLOCKS = '1' } else { Remove-Item Env:TARTARUS_GI_BAKE_LIVE_CLOCKS -ErrorAction SilentlyContinue }
$args2 = @('--bake-gi', 'project/scenes/Forest.json')
if ($Only) { $args2 += @('--gi-only', "`"$Only`"") }
if ($Doors) { $args2 += @('--bake-gi-doors', $Doors) } # e.g. hidden (needs an exe with dev/gi-doors-hidden 42ba4496)
$start = Get-Date
$p = Start-Process "$g\build\Release\TartarusEngine.exe" -WorkingDirectory $g -WindowStyle Hidden -PassThru `
    -ArgumentList $args2 -RedirectStandardOutput "$k\$Tag.stdout.txt" -RedirectStandardError "$k\$Tag.stderr.txt"
$h = $p.Handle  # keeps the exit code readable
try { $p.PriorityClass = 'BelowNormal' } catch {}
# Watchdog: a run whose bake is done ("[GI bake] done in") but whose process hangs on shutdown (seen 2026-11-24 on
# ab-live-lr2-b: idle, CPU flat) is killed after 120 s; its .gi is complete. A run past -TimeoutMin is killed as failed.
$hung = ''
$doneAt = $null
while (-not $p.WaitForExit(5000)) {
    if (-not $doneAt -and (Select-String -LiteralPath "$k\$Tag.stdout.txt" -SimpleMatch '[GI bake] done in' -Quiet)) { $doneAt = Get-Date }
    if ($doneAt -and ((Get-Date) - $doneAt).TotalSeconds -gt 120) { $hung = 'shutdown-hang-killed (bake done)'; Stop-Process -Id $p.Id -Force; $p.WaitForExit(); break }
    if (((Get-Date) - $start).TotalMinutes -gt $TimeoutMin) { $hung = "timeout-killed after $TimeoutMin min"; Stop-Process -Id $p.Id -Force; $p.WaitForExit(); break }
}
$end = Get-Date
$code = if ($hung -like 'shutdown*') { 0 } else { $p.ExitCode }
$changed = (git -C $g status --porcelain -- project/scenes/Forest.gi)
$hash = ''
if ($changed) {
    Copy-Item -LiteralPath $gi -Destination "$k\Forest.$Tag.gi" -Force
    $hash = (Get-FileHash "$k\Forest.$Tag.gi" -Algorithm SHA256).Hash
}
git -C $g checkout -- project/scenes/Forest.gi
@(
    "DESKTOP-GQKO56V, RAM 2133 (XMP off), MSVC 14.52.37025; runner HEAD $(git -C $g rev-parse --short HEAD)"
    "tag $Tag exit $code only '$Only' live $([bool]$Live) $hung"
    "start $($start.ToString('o'))"
    "end   $($end.ToString('o'))"
    "wall  {0:N1} s" -f ($end - $start).TotalSeconds
    "gi written: $([bool]$changed) sha256 $hash"
    "D: free after {0:N2} GB" -f ((Get-PSDrive D).Free / 1GB)
) | Out-File "$k\$Tag.summary.txt" -Encoding utf8
