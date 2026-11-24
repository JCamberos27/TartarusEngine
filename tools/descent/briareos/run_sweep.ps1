# W6 sweep GPU run. Caller holds locks\perf (owner.txt names W6) after the SM's "GPU GO". Hidden, BelowNormal, muted.
# One run writes BOTH views per waypoint (NN_<id>_full.png and NN_<id>_gi.png). Does NOT switch wt-gpu: the SM puts it at the sha to sweep.
# Usage: run_sweep.ps1 -Out D:\...\sweep_baseline [-Tour ...\sweep_tour.json] [-TimeoutMin 30]
param([Parameter(Mandatory)][string]$Out, [string]$Tour = 'D:\Jacob\tblocks\descent\house-polish\briareos\sweep\sweep_tour.json', [int]$TimeoutMin = 30, [switch]$SceneSwap)
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'
$exe = "$g\build\Release\TartarusEngine.exe"
$machine = 'DESKTOP-GQKO56V, RAM 2133 (XMP off)'
if (-not (Test-Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt') -or -not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W6' -Quiet)) { throw "locks\perf is not held by W6: STOP" }
$dirty = @(git -C $g status --porcelain --untracked-files=no)
if ($SceneSwap) { if ($dirty | Where-Object { $_ -notmatch 'project/scenes/Forest\.json$' }) { throw "wt-gpu dirty beyond Forest.json: STOP" } }
elseif ($dirty) { throw "wt-gpu not clean: STOP" }
if ((Get-PSDrive D).Free / 1GB -lt 100) { throw "D: under the 100 GB floor" }
$other = @(Get-Process TartarusEngine -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path -notlike "$g\*" })
if ($other.Count) { throw "another TartarusEngine is running (pid $($other.Id -join ',')): Jacob's editor, no GPU" }
New-Item -ItemType Directory -Force $Out | Out-Null
$sha = (git -C $g rev-parse --short=8 HEAD).Trim()
$log = "$Out\sweep_summary.txt"
"$machine; wt-gpu at $sha; tour $Tour; NOT perf numbers if Jacob was using the PC" | Set-Content $log
$env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
$t0 = Get-Date
$p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru `
    -ArgumentList '--project', "$g\project", '--gi-tour', $Tour, '--gi-tour-view', 'full', '--out', "$Out\png", '--perf-res', '1920x1080' `
    -RedirectStandardOutput "$Out\stdout.txt" -RedirectStandardError "$Out\stderr.txt"
$null = $p.Handle
try { $p.PriorityClass = 'BelowNormal' } catch {}
if (-not $p.WaitForExit($TimeoutMin * 60000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(); $r = 'timeout-killed' }
else { $r = (Select-String -Path "$Out\stdout.txt" -Pattern '\[SmokeTest\] (PASS|FAIL)' | Select-Object -Last 1).Line }
"sweep: exit={0} wall={1:N0}s | {2}" -f $p.ExitCode, ((Get-Date) - $t0).TotalSeconds, $r | Add-Content $log
"png files: $((Get-ChildItem "$Out\png" -Filter *.png -ErrorAction SilentlyContinue | Measure-Object).Count)" | Add-Content $log
Get-Content $log
