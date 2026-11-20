# Motion matching overhaul: the one test entry point (docs/MOTION_MATCHING_HANDOFF.md, "Test protocol").
#
#   powershell -ExecutionPolicy Bypass -File tools\mm_smoke.ps1 [-Quick | -Full] [scenario ...] [-Runs N] [-PerfBench <scene.json>]
#       [-TraceDir <dir>] [-UnitFilter <filter>] [-AllowErrors <regex>] [-SkipBuild] [-SkipUnit] [-SkipSmoke] [-Sound]
#       [-TimeoutMinutes N] [-Legs <names>] [-Exe <path>] [-LogDir <dir>] [-Parallel N]
#
# Presets (docs/MOTION_MATCHING_HANDOFF.md, "Fast iteration"):
#   -Quick  unit, mmplayer, mmstress (~3-4 min): the check before committing motion matching work. Scenarios named after it
#           are added - `-Quick mmview mmdb` when the bake or the database changed.
#   -Full   the whole pass: unit, smoke, testbed, npcloco, stopsteps, feet, labloco, mmview, mmdb, mmplayer, npcperf,
#           mmstress and the perf bench (tests\perf-scenes\perf_stress.json). Once per PR, before merging. The six old-locomotion
#           scenarios (testbed .. labloco) otherwise run only when a change touches what they share (foot IK, Player,
#           FirstPersonBody's legacy path); Phase 6 retires them.
#
# -Legs: only these legs of testbed / mmplayer / mmstress (TARTARUS_TESTBED_LEGS: comma-separated, * wildcards; each runs from
#   its part's last placement leg), e.g. -Legs "M sprint*,M jog,A walk". Checks naming a leg that didn't run are skipped.
#   The fast loop for experiments; the whole course is for the final pass (docs/MOTION_MATCHING_HANDOFF.md, "Fast iteration").
# -Exe: run this engine build instead of build\Release\TartarusEngine.exe (implies -SkipBuild) - copy the exe to a folder per
#   experiment and run 2-3 scripts at once, each with its own -LogDir (the copy runs from the repo, as the original does).
#
# -Parallel N (default 3): up to N engine runs at once (the unit tests, the smoke scenes and the npc scenarios are independent;
#   a run's results don't depend on what else runs - its game steps a fixed 1/60 s). The timing checks run alone: `mmdb`, `npcperf`
#   (search time) and the perf bench. -Parallel 1 runs everything one at a time, in order.
#
# Builds Release, then runs:
#   1. --unit-tests [UnitFilter]
#   2. --smoke-test tests\smoke-scenes
#   3. --npc-test <scenario> for every scenario given, Runs times each
#   4. --perf-bench <PerfBench>, when given
# Every run is visible (TARTARUS_SHOW=1; harness runs are hidden without it) at unlocked FPS (TARTARUS_UNLOCKED_FPS=1), muted unless -Sound.
# Release is a GUI-subsystem build, so each run is a Start-Process -PassThru + WaitForExit() with stdout redirected to
# its log. Each npc run gets NPC_TEST_TRACE=<TraceDir>\<scenario>[_<k>] (default <LogDir>\traces): testbed and labloco
# write trace.csv there, and labloco's one check counts the traced frames, so it fails without it.
# Logs land in work\mm_smoke\<timestamp>\ (git-ignored). Prints a pass/fail table; exit code 1 on any failure.
# -AllowErrors: a run whose only failure is "N new log error(s)", every one of them matching this regex, is PASS* (and
# doesn't fail the script). For a known broken local asset, never a default: e.g. -AllowErrors 'worklight_hum_1|T_Spider_AO'.
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Scenarios = @(),
    [int]$Runs = 1,
    [string]$PerfBench = '',
    [string]$TraceDir = '',
    [string]$UnitFilter = '',
    [string]$AllowErrors = '',
    [switch]$SkipBuild,
    [switch]$SkipUnit,
    [switch]$SkipSmoke,
    [switch]$Sound,
    [int]$TimeoutMinutes = 30,
    [string]$LogDir = '',
    [string]$Legs = '',
    [string]$Exe = '',
    [int]$Parallel = 3,
    [switch]$Quick,
    [switch]$Full
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($Quick -and $Full) { Write-Host '-Quick and -Full are alternatives: pick one' -ForegroundColor Red; exit 1 }
if ($Quick) {
    $SkipSmoke = $true
    $Scenarios = @('mmplayer', 'mmstress') + @($Scenarios | Where-Object { $_ -notin @('mmplayer', 'mmstress') })
}
if ($Full) {
    $SkipUnit = $false; $SkipSmoke = $false
    $all12 = @('testbed', 'npcloco', 'stopsteps', 'feet', 'labloco', 'mmview', 'mmdb', 'mmplayer', 'mmstress', 'npcperf')
    $Scenarios = $all12 + @($Scenarios | Where-Object { $_ -notin $all12 })
    if (-not $PerfBench) { $PerfBench = 'tests\perf-scenes\perf_stress.json' }
}
# (Not "$exe": PowerShell's names ignore case, so it would be the -Exe parameter itself, always set.)
$engineExe = Join-Path $repo 'build\Release\TartarusEngine.exe'
if ($Exe) { $engineExe = (Resolve-Path $Exe).Path; $SkipBuild = $true }
if (-not $LogDir) { $LogDir = Join-Path $repo ('work\mm_smoke\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = (Resolve-Path $LogDir).Path
if (-not $TraceDir) { $TraceDir = Join-Path $LogDir 'traces' }
if ($TraceDir) {
    New-Item -ItemType Directory -Force -Path $TraceDir | Out-Null
    $TraceDir = (Resolve-Path $TraceDir).Path
}

$results = New-Object System.Collections.Generic.List[object]

# Whether a failed run failed only on log errors that -AllowErrors names: every failing [SmokeTest] line's cause is just
# "N new log error(s)", and every [error] line on stderr matches.
function Test-Tolerated($failedSummaries, $errors) {
    if (-not $AllowErrors -or -not $errors -or $errors.Count -eq 0) { return $false }
    foreach ($line in $failedSummaries) {
        if ($line -notmatch 'cause: \d+ new log error\(s\); *$') { return $false }
    }
    foreach ($e in $errors) { if ($e -notmatch $AllowErrors) { return $false } }
    return $true
}

function Add-Result($step, $ok, $exit, $detail, $wall, $log, $errors, $tolerated = $false, $order = 0) {
    if (-not $ok -and $errors) {
        $shown = @($errors | Select-Object -Unique | Select-Object -First 2)
        $detail += $(if ($tolerated) { '; allowed: ' } else { '; stderr: ' }) + ($shown -join ' | ')
    }
    $results.Add([pscustomobject]@{
        Order = $order; Step = $step; Result = $(if ($ok) { 'PASS' } elseif ($tolerated) { 'PASS*' } else { 'FAIL' }); Exit = $exit
        'Wall s' = [math]::Round($wall, 1); Detail = $detail; Log = $(if ($log) { Split-Path -Leaf $log } else { '' })
    })
}

# One engine run: visible, unlocked FPS, stdout + stderr to <name>.log. Start-Engine launches it (the environment is the
# process's at launch, so it is set, the run started, and put back); Wait-Engine returns @{Exit; Lines; Errors; Wall; TimedOut; Log}.
function Start-Engine([string]$name, [string[]]$engineArgs, [hashtable]$extraEnv) {
    $log = Join-Path $LogDir "$name.log"
    $err = Join-Path $LogDir "$name.stderr.log"
    $set = @{ TARTARUS_UNLOCKED_FPS = '1'; TARTARUS_HIDDEN = $null; TARTARUS_SHOW = '1'; TARTARUS_MUTE = $(if ($Sound) { $null } else { '1' }) }
    if ($extraEnv) { foreach ($k in $extraEnv.Keys) { $set[$k] = $extraEnv[$k] } }
    $saved = @{}
    foreach ($k in $set.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
        [Environment]::SetEnvironmentVariable($k, $set[$k], 'Process')
    }
    Write-Host ("-> {0}: TartarusEngine.exe {1}" -f $name, ($engineArgs -join ' ')) -ForegroundColor Cyan
    try {
        $quoted = $engineArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
        $p = Start-Process -FilePath $engineExe -ArgumentList $quoted -WorkingDirectory $repo -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError $err
        $null = $p.Handle # keeps ExitCode readable after the process is gone
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    return @{ Name = $name; P = $p; Log = $log; Err = $err; Watch = [Diagnostics.Stopwatch]::StartNew() }
}
function Wait-Engine($h) {
    $timedOut = $false
    $left = [math]::Max(0, $TimeoutMinutes * 60 * 1000 - $h.Watch.ElapsedMilliseconds)
    if (-not $h.P.WaitForExit([int]$left)) {
        $timedOut = $true
        Stop-Process -Id $h.P.Id -Force -ErrorAction SilentlyContinue
        $h.P.WaitForExit()
    }
    $h.Watch.Stop()
    $lines = @()
    $errors = @()
    if (Test-Path $h.Log) { $lines = @(Get-Content -LiteralPath $h.Log -Encoding UTF8) }
    if (Test-Path $h.Err) {
        # The engine's [error] lines go to stderr: they are what a "new log error(s)" cause counts.
        $errors = @(Get-Content -LiteralPath $h.Err -Encoding UTF8 | Where-Object { $_ -match '^\[error\]' })
        if ((Get-Item $h.Err).Length -eq 0) { Remove-Item $h.Err }
    }
    return @{ Exit = $h.P.ExitCode; Lines = $lines; Errors = $errors; Wall = $h.Watch.Elapsed.TotalSeconds; TimedOut = $timedOut; Log = $h.Log }
}

# Build.
if (-not $SkipBuild) {
    Write-Host '-> build: cmake --build build --config Release --parallel' -ForegroundColor Cyan
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $buildLog = Join-Path $LogDir 'build.log'
    & cmake --build (Join-Path $repo 'build') --config Release --parallel *> $buildLog
    $code = $LASTEXITCODE
    $watch.Stop()
    Add-Result 'build' ($code -eq 0) $code '' $watch.Elapsed.TotalSeconds $buildLog $null $false -1
    if ($code -ne 0) {
        Get-Content $buildLog | Select-String -Pattern 'error' | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
        $results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
        exit 1
    }
}
if (-not (Test-Path $engineExe)) { Write-Host "No engine at $engineExe" -ForegroundColor Red; exit 1 }

# The steps: each an engine run and how its log reads.
$steps = New-Object System.Collections.Generic.List[object]
function Add-Step($name, $engineArgs, $envs, $kind, [bool]$alone = $false) {
    $steps.Add(@{ Order = $steps.Count; Name = $name; Args = $engineArgs; Env = $envs; Kind = $kind; Alone = $alone })
}
if (-not $SkipUnit) {
    $engineArgs = @('--unit-tests')
    if ($UnitFilter) { $engineArgs += $UnitFilter }
    Add-Step 'unit' $engineArgs $null 'unit'
}
if (-not $SkipSmoke) { Add-Step 'smoke' @('--smoke-test', 'tests\smoke-scenes') $null 'smoke' }
foreach ($scenario in $Scenarios) {
    for ($k = 1; $k -le $Runs; ++$k) {
        $name = if ($Runs -gt 1) { "npc_${scenario}_$k" } else { "npc_$scenario" }
        $envs = @{}
        if ($Legs) { $envs['TARTARUS_TESTBED_LEGS'] = $Legs }
        if ($TraceDir) {
            $dir = Join-Path $TraceDir $(if ($Runs -gt 1) { "${scenario}_$k" } else { $scenario })
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            $envs['NPC_TEST_TRACE'] = $dir
        }
        $engineScenario = $scenario
        # (Timed runs alone: mmdb times the search, npcperf the dummies' cost and the frame.)
        Add-Step $name @('--npc-test', $engineScenario) $envs 'npc' ($engineScenario -eq 'mmdb' -or $engineScenario -eq 'npcperf')
    }
}
if ($PerfBench) { Add-Step 'perf' @('--perf-bench', $PerfBench) $null 'perf' $true }

function Read-Step($step, $r) {
    $name = $step.Name
    switch ($step.Kind) {
        'unit' {
            $summary = $r.Lines | Where-Object { $_ -match '^\[UnitTest\] \d+ checks, \d+ failure' } | Select-Object -Last 1
            $failed = @($r.Lines | Where-Object { $_ -match '^\[UnitTest\] FAIL' })
            $ok = $r.Exit -eq 0 -and $summary -and $failed.Count -eq 0 -and -not $r.TimedOut
            $detail = if ($r.TimedOut) { 'timed out' } elseif ($summary) { $summary -replace '^\[UnitTest\] ', '' } else { 'no summary line' }
            if ($failed.Count) { $detail += '; ' + (($failed | Select-Object -First 3) -join '; ') }
            Add-Result 'unit' $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $false $step.Order
        }
        'smoke' {
            $scenes = @($r.Lines | Where-Object { $_ -match '^\[SmokeTest\] (PASS|FAIL)  .* wall=' })
            $bad = @($scenes | Where-Object { $_ -match '^\[SmokeTest\] FAIL' })
            $ok = $r.Exit -eq 0 -and $scenes.Count -gt 0 -and $bad.Count -eq 0 -and -not $r.TimedOut
            $detail = if ($r.TimedOut) { 'timed out' } else { "$($scenes.Count - $bad.Count)/$($scenes.Count) scenes" }
            if ($bad.Count) { $detail += '; ' + (($bad | ForEach-Object { ($_ -split '\s+')[2] }) -join ', ') }
            $tol = -not $ok -and -not $r.TimedOut -and $scenes.Count -gt 0 -and $bad.Count -gt 0 -and (Test-Tolerated $bad $r.Errors)
            Add-Result 'smoke' $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $tol $step.Order
        }
        'npc' {
            $checks = @($r.Lines | Where-Object { $_ -match '^\[NpcTest\] (PASS|FAIL) ' })
            $bad = @($checks | Where-Object { $_ -match '^\[NpcTest\] FAIL ' })
            $summary = $r.Lines | Where-Object { $_ -match '^\[SmokeTest\] (PASS|FAIL)  .* wall=' } | Select-Object -Last 1
            $ok = $r.Exit -eq 0 -and $summary -match '^\[SmokeTest\] PASS' -and $bad.Count -eq 0 -and -not $r.TimedOut
            $detail = if ($r.TimedOut) { 'timed out' } else { "$($checks.Count - $bad.Count)/$($checks.Count) checks" }
            $skipped = @($r.Lines | Where-Object { $_ -match '^\[NpcTest\] SKIP ' }).Count
            if ($skipped -gt 0) { $detail += " ($skipped skipped)" }
            if ($summary -match '(fps=\d+ unfocused=\d+)') { $detail += "  $($Matches[1])" }
            if ($summary -match 'cause: (.*)$') { $detail += "; $($Matches[1])" }
            if ($bad.Count) { $detail += '; ' + (($bad | Select-Object -First 3 | ForEach-Object { $_ -replace '^\[NpcTest\] FAIL ', '' }) -join '; ') }
            $tol = -not $ok -and -not $r.TimedOut -and $bad.Count -eq 0 -and $checks.Count -gt 0 -and $summary -and
                   (Test-Tolerated @($summary) $r.Errors)
            Add-Result $name $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $tol $step.Order
        }
        'perf' {
            $phases = @($r.Lines | Where-Object { $_ -match '^\[PerfBench\] \S.*frames=\d+  avg=' })
            $summary = $r.Lines | Where-Object { $_ -match '^\[SmokeTest\] (PASS|FAIL)  .* wall=' } | Select-Object -Last 1
            $ok = $r.Exit -eq 0 -and $phases.Count -gt 0 -and $summary -match '^\[SmokeTest\] PASS' -and -not $r.TimedOut
            $detail = if ($r.TimedOut) { 'timed out' } else {
                ($phases | ForEach-Object { if ($_ -match '\((\w+)\)\s+frames=\d+\s+avg=([\d.]+) ms') { "$($Matches[1]) $($Matches[2]) ms" } }) -join ', '
            }
            if ($summary -match 'cause: (.*)$') { $detail += "; $($Matches[1])" }
            $tol = -not $ok -and -not $r.TimedOut -and $phases.Count -gt 0 -and $summary -and (Test-Tolerated @($summary) $r.Errors)
            Add-Result 'perf' $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $tol $step.Order
        }
    }
}

# The pool: up to -Parallel runs at once, a timing step (Alone) only with nothing else running.
$width = [math]::Max(1, $Parallel)
$pending = New-Object System.Collections.Generic.Queue[object]
foreach ($st in ($steps | Where-Object { -not $_.Alone })) { $pending.Enqueue($st) }
$aloneSteps = @($steps | Where-Object { $_.Alone })
$running = New-Object System.Collections.Generic.List[object]
$all = [Diagnostics.Stopwatch]::StartNew()
while ($pending.Count -gt 0 -or $running.Count -gt 0) {
    while ($pending.Count -gt 0 -and $running.Count -lt $width) {
        $st = $pending.Dequeue()
        $running.Add(@{ Step = $st; H = (Start-Engine $st.Name $st.Args $st.Env) })
    }
    $done = @($running | Where-Object { $_.H.P.HasExited -or $_.H.Watch.ElapsedMilliseconds -gt $TimeoutMinutes * 60 * 1000 })
    if ($done.Count -eq 0) { Start-Sleep -Milliseconds 250; continue }
    foreach ($d in $done) { [void]$running.Remove($d); Read-Step $d.Step (Wait-Engine $d.H) }
}
foreach ($st in $aloneSteps) { Read-Step $st (Wait-Engine (Start-Engine $st.Name $st.Args $st.Env)) }
$all.Stop()
$results = [System.Collections.Generic.List[object]]@($results | Sort-Object Order)

Write-Host ''
$results | Select-Object Step, Result, Exit, 'Wall s', Detail, Log | Format-Table -AutoSize -Wrap | Out-String -Width 240 | Write-Host
Write-Host ("Engine runs: {0:N0} s wall (-Parallel {1})" -f $all.Elapsed.TotalSeconds, $width)
Write-Host "Logs: $LogDir"
if ($TraceDir) { Write-Host "Traces: $TraceDir" }
if (@($results | Where-Object { $_.Result -eq 'PASS*' }).Count) { Write-Host "PASS*: failed only on log errors -AllowErrors names" -ForegroundColor Yellow }
$failures = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
if ($failures) { Write-Host "$failures step(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'All steps passed' -ForegroundColor Green
exit 0
