# W6 sweep GPU run. Hidden, BelowNormal, muted. One run writes BOTH views per waypoint (NN_<id>_full.png and NN_<id>_gi.png).
# Does NOT switch the runner: put it at the sha to sweep first.
# Usage: run_sweep.ps1 -Out <dir> -Tour <tour.json> [-Runner <repo worktree with build\Release>] [-TimeoutMin 30] [-SceneSwap]
#   -Runner   the worktree whose build\Release\TartarusEngine.exe runs (default: $env:DESCENT_RUNNER, else <team root>\wt-gpu, where the
#             team root is $env:DESCENT_ROOT or D:\Jacob\tblocks\descent). On a machine without the team lock folder (home PC) pass
#             -NoLock and use your own checkout as -Runner.
#   -LockOwner text the locks\perf\owner.txt must contain (default W6); the SM hands the lock out. -NoLock skips the check.
param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$Tour, [string]$Runner = '', [int]$TimeoutMin = 30,
      [switch]$SceneSwap, [switch]$NoLock, [string]$LockOwner = 'W6')
$ErrorActionPreference = 'Stop'
$root = if ($env:DESCENT_ROOT) { $env:DESCENT_ROOT } else { 'D:\Jacob\tblocks\descent' }
$g = if ($Runner) { $Runner } elseif ($env:DESCENT_RUNNER) { $env:DESCENT_RUNNER } else { "$root\wt-gpu" }
$exe = "$g\build\Release\TartarusEngine.exe"
if (-not (Test-Path $exe)) { throw "no engine at $exe" }
$machine = if ($env:COMPUTERNAME -eq 'DESKTOP-GQKO56V') { 'DESKTOP-GQKO56V, RAM 2133 (XMP off)' } else { $env:COMPUTERNAME }
$lock = "$root\locks\perf\owner.txt"
if (-not $NoLock) {
    if (-not (Test-Path $lock) -or -not (Select-String -Path $lock -Pattern $LockOwner -Quiet)) { throw "locks\perf is not held by ${LockOwner}: STOP" }
}
$dirty = @(git -C $g status --porcelain --untracked-files=no)
if ($SceneSwap) { if ($dirty | Where-Object { $_ -notmatch 'project/scenes/Forest\.json$' }) { throw "runner dirty beyond Forest.json: STOP" } }
elseif ($dirty) { throw "runner not clean: STOP" }
$drive = (Get-Item $g).PSDrive
if ($drive.Free / 1GB -lt 100) { throw "$($drive.Name): under the 100 GB floor" }
$other = @(Get-Process TartarusEngine -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path -notlike "$g\*" })
if ($other.Count) { throw "another TartarusEngine is running (pid $($other.Id -join ',')): Jacob's editor, no GPU" }
New-Item -ItemType Directory -Force $Out | Out-Null
$sha = (git -C $g rev-parse --short=8 HEAD).Trim()
$log = "$Out\sweep_summary.txt"
"$machine; runner $g at $sha; tour $Tour; NOT perf numbers if Jacob was using the PC" | Set-Content $log
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
