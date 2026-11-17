# Motion matching overhaul: the one test entry point (docs/MOTION_MATCHING_HANDOFF.md, "Test protocol").
#
#   powershell -ExecutionPolicy Bypass -File tools\mm_smoke.ps1 [scenario ...] [-Runs N] [-PerfBench <scene.json>]
#       [-TraceDir <dir>] [-UnitFilter <filter>] [-AllowErrors <regex>] [-SkipBuild] [-SkipUnit] [-SkipSmoke] [-Sound]
#       [-TimeoutMinutes N]
#
# Builds Release, then runs, in order and one at a time:
#   1. --unit-tests [UnitFilter]
#   2. --smoke-test tests\smoke-scenes
#   3. --npc-test <scenario> for every scenario given, Runs times each
#   4. --perf-bench <PerfBench>, when given
# Every run is visible (TARTARUS_HIDDEN is cleared) at unlocked FPS (TARTARUS_UNLOCKED_FPS=1), muted unless -Sound.
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
    [string]$LogDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\Release\TartarusEngine.exe'
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

function Add-Result($step, $ok, $exit, $detail, $wall, $log, $errors, $tolerated = $false) {
    if (-not $ok -and $errors) {
        $shown = @($errors | Select-Object -Unique | Select-Object -First 2)
        $detail += $(if ($tolerated) { '; allowed: ' } else { '; stderr: ' }) + ($shown -join ' | ')
    }
    $results.Add([pscustomobject]@{
        Step = $step; Result = $(if ($ok) { 'PASS' } elseif ($tolerated) { 'PASS*' } else { 'FAIL' }); Exit = $exit
        'Wall s' = [math]::Round($wall, 1); Detail = $detail; Log = $(if ($log) { Split-Path -Leaf $log } else { '' })
    })
}

# One engine run: visible, unlocked FPS, stdout + stderr to <name>.log. Returns @{Exit; Lines; Wall; TimedOut}.
function Invoke-Engine([string]$name, [string[]]$engineArgs, [hashtable]$extraEnv) {
    $log = Join-Path $LogDir "$name.log"
    $err = Join-Path $LogDir "$name.stderr.log"
    $set = @{ TARTARUS_UNLOCKED_FPS = '1'; TARTARUS_HIDDEN = $null; TARTARUS_MUTE = $(if ($Sound) { $null } else { '1' }) }
    if ($extraEnv) { foreach ($k in $extraEnv.Keys) { $set[$k] = $extraEnv[$k] } }
    $saved = @{}
    foreach ($k in $set.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
        [Environment]::SetEnvironmentVariable($k, $set[$k], 'Process')
    }
    Write-Host ("-> {0}: TartarusEngine.exe {1}" -f $name, ($engineArgs -join ' ')) -ForegroundColor Cyan
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    try {
        $quoted = $engineArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
        $p = Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $repo -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError $err
        $null = $p.Handle # keeps ExitCode readable after the process is gone
        if (-not $p.WaitForExit($TimeoutMinutes * 60 * 1000)) {
            $timedOut = $true
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $p.WaitForExit()
        }
        $exit = $p.ExitCode
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    $watch.Stop()
    $lines = @()
    $errors = @()
    if (Test-Path $log) { $lines = @(Get-Content -LiteralPath $log -Encoding UTF8) }
    if (Test-Path $err) {
        # The engine's [error] lines go to stderr: they are what a "new log error(s)" cause counts.
        $errors = @(Get-Content -LiteralPath $err -Encoding UTF8 | Where-Object { $_ -match '^\[error\]' })
        if ((Get-Item $err).Length -eq 0) { Remove-Item $err }
    }
    return @{ Exit = $exit; Lines = $lines; Errors = $errors; Wall = $watch.Elapsed.TotalSeconds; TimedOut = $timedOut; Log = $log }
}

# Build.
if (-not $SkipBuild) {
    Write-Host '-> build: cmake --build build --config Release --parallel' -ForegroundColor Cyan
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $buildLog = Join-Path $LogDir 'build.log'
    & cmake --build (Join-Path $repo 'build') --config Release --parallel *> $buildLog
    $code = $LASTEXITCODE
    $watch.Stop()
    Add-Result 'build' ($code -eq 0) $code '' $watch.Elapsed.TotalSeconds $buildLog
    if ($code -ne 0) {
        Get-Content $buildLog | Select-String -Pattern 'error' | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
        $results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
        exit 1
    }
}
if (-not (Test-Path $exe)) { Write-Host "No engine at $exe" -ForegroundColor Red; exit 1 }

# Unit tests.
if (-not $SkipUnit) {
    $engineArgs = @('--unit-tests')
    if ($UnitFilter) { $engineArgs += $UnitFilter }
    $r = Invoke-Engine 'unit' $engineArgs $null
    $summary = $r.Lines | Where-Object { $_ -match '^\[UnitTest\] \d+ checks, \d+ failure' } | Select-Object -Last 1
    $failed = @($r.Lines | Where-Object { $_ -match '^\[UnitTest\] FAIL' })
    $ok = $r.Exit -eq 0 -and $summary -and $failed.Count -eq 0 -and -not $r.TimedOut
    $detail = if ($r.TimedOut) { 'timed out' } elseif ($summary) { $summary -replace '^\[UnitTest\] ', '' } else { 'no summary line' }
    if ($failed.Count) { $detail += '; ' + (($failed | Select-Object -First 3) -join '; ') }
    Add-Result 'unit' $ok $r.Exit $detail $r.Wall $r.Log $r.Errors
}

# Smoke scenes.
if (-not $SkipSmoke) {
    $r = Invoke-Engine 'smoke' @('--smoke-test', 'tests\smoke-scenes') $null
    $scenes = @($r.Lines | Where-Object { $_ -match '^\[SmokeTest\] (PASS|FAIL)  .* wall=' })
    $bad = @($scenes | Where-Object { $_ -match '^\[SmokeTest\] FAIL' })
    $ok = $r.Exit -eq 0 -and $scenes.Count -gt 0 -and $bad.Count -eq 0 -and -not $r.TimedOut
    $detail = if ($r.TimedOut) { 'timed out' } else { "$($scenes.Count - $bad.Count)/$($scenes.Count) scenes" }
    if ($bad.Count) { $detail += '; ' + (($bad | ForEach-Object { ($_ -split '\s+')[2] }) -join ', ') }
    $tol = -not $ok -and -not $r.TimedOut -and $scenes.Count -gt 0 -and $bad.Count -gt 0 -and (Test-Tolerated $bad $r.Errors)
    Add-Result 'smoke' $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $tol
}

# NPC test scenarios.
foreach ($scenario in $Scenarios) {
    for ($k = 1; $k -le $Runs; ++$k) {
        $name = if ($Runs -gt 1) { "npc_${scenario}_$k" } else { "npc_$scenario" }
        $envs = @{}
        if ($TraceDir) {
            $dir = Join-Path $TraceDir $(if ($Runs -gt 1) { "${scenario}_$k" } else { $scenario })
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            $envs['NPC_TEST_TRACE'] = $dir
        }
        $r = Invoke-Engine $name @('--npc-test', $scenario) $envs
        $checks = @($r.Lines | Where-Object { $_ -match '^\[NpcTest\] (PASS|FAIL) ' })
        $bad = @($checks | Where-Object { $_ -match '^\[NpcTest\] FAIL ' })
        $summary = $r.Lines | Where-Object { $_ -match '^\[SmokeTest\] (PASS|FAIL)  .* wall=' } | Select-Object -Last 1
        $ok = $r.Exit -eq 0 -and $summary -match '^\[SmokeTest\] PASS' -and $bad.Count -eq 0 -and -not $r.TimedOut
        $detail = if ($r.TimedOut) { 'timed out' } else { "$($checks.Count - $bad.Count)/$($checks.Count) checks" }
        if ($summary -match '(fps=\d+ unfocused=\d+)') { $detail += "  $($Matches[1])" }
        if ($summary -match 'cause: (.*)$') { $detail += "; $($Matches[1])" }
        if ($bad.Count) { $detail += '; ' + (($bad | Select-Object -First 3 | ForEach-Object { $_ -replace '^\[NpcTest\] FAIL ', '' }) -join '; ') }
        $tol = -not $ok -and -not $r.TimedOut -and $bad.Count -eq 0 -and $checks.Count -gt 0 -and $summary -and
               (Test-Tolerated @($summary) $r.Errors)
        Add-Result $name $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $tol
    }
}

# Perf bench.
if ($PerfBench) {
    $r = Invoke-Engine 'perf' @('--perf-bench', $PerfBench) $null
    $phases = @($r.Lines | Where-Object { $_ -match '^\[PerfBench\] \S.*frames=\d+  avg=' })
    $summary = $r.Lines | Where-Object { $_ -match '^\[SmokeTest\] (PASS|FAIL)  .* wall=' } | Select-Object -Last 1
    $ok = $r.Exit -eq 0 -and $phases.Count -gt 0 -and $summary -match '^\[SmokeTest\] PASS' -and -not $r.TimedOut
    $detail = if ($r.TimedOut) { 'timed out' } else {
        ($phases | ForEach-Object { if ($_ -match '\((\w+)\)\s+frames=\d+\s+avg=([\d.]+) ms') { "$($Matches[1]) $($Matches[2]) ms" } }) -join ', '
    }
    if ($summary -match 'cause: (.*)$') { $detail += "; $($Matches[1])" }
    $tol = -not $ok -and -not $r.TimedOut -and $phases.Count -gt 0 -and $summary -and (Test-Tolerated @($summary) $r.Errors)
    Add-Result 'perf' $ok $r.Exit $detail $r.Wall $r.Log $r.Errors $tol
}

Write-Host ''
$results | Format-Table -AutoSize -Wrap | Out-String -Width 240 | Write-Host
Write-Host "Logs: $LogDir"
if ($TraceDir) { Write-Host "Traces: $TraceDir" }
if (@($results | Where-Object { $_.Result -eq 'PASS*' }).Count) { Write-Host "PASS*: failed only on log errors -AllowErrors names" -ForegroundColor Yellow }
$failures = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
if ($failures) { Write-Host "$failures step(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'All steps passed' -ForegroundColor Green
exit 0
