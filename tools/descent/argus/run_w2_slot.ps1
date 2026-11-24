# W2 verify slot on wt-gpu at the detached verify sha (dev/w2-verify2 4f8bc1aa). Caller holds locks\perf (owner W2) after the SM's GPU GO.
# Hidden, BelowNormal, muted. Restores every file it touches in wt-gpu (M_Mirror.mat, Forest.gi). Output: gi-p2\argus\slot\out\
#  1. guard: --bake-gi --gi-only child's bedroom 2 / living room 7 / living room 8 (run-bake.ps1; "kept on the face" log lines)
#  2. main tour with M_Mirror _Metallic 1.0 (the matte reverted TEMPORARILY): 4 mirrors, kitchen sink (2), front + east facade (6); probe log + dumps
#  3. moon A/B: tour_moon at TARTARUS_VOL_INSCATTER_CAP = 0 (off), 1, 3 (the default 2 is in the main tour's 251/253 poses)
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'; $exe = "$g\build\Release\TartarusEngine.exe"; $s = 'D:\Jacob\tblocks\descent\gi-p2\argus\slot'; $o = "$s\out"
$mat = "$g\project\assets\Environments\SuburbNeighborhoodHousePack\Materials\M_Mirror.mat"
if (-not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W2' -Quiet)) { throw 'locks\perf is not held by W2: STOP' }
if (git -C $g status --porcelain --untracked-files=no) { throw 'wt-gpu not clean: STOP' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'another TartarusEngine is running: STOP' }
New-Item -ItemType Directory -Force $o | Out-Null
"wt-gpu at $(git -C $g rev-parse --short=8 HEAD); start $((Get-Date).ToUniversalTime().ToString('o'))" | Set-Content "$o\slot_summary.txt"
function Run-Tour($tour, $dir, $timeoutMin) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru `
        -ArgumentList '--project', "$g\project", '--gi-tour', $tour, '--gi-tour-view', 'full', '--out', "$dir\png", '--perf-res', '1920x1080' `
        -RedirectStandardOutput "$dir\stdout.txt" -RedirectStandardError "$dir\stderr.txt"
    $null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
    if (-not $p.WaitForExit($timeoutMin * 60000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(); $r = 'timeout-killed' }
    else { $r = "exit=$($p.ExitCode) " + (Select-String -Path "$dir\stdout.txt" -Pattern '\[SmokeTest\] (PASS|FAIL)' | Select-Object -Last 1).Line }
    "$dir : $r" | Add-Content "$o\slot_summary.txt"
}
try {
    # 1. guard bakes
    foreach ($b in @(@('guard-cb2', "child's bedroom 2"), @('guard-lr7', 'living room 7'), @('guard-lr8', 'living room 8'))) {
        & 'D:\Jacob\tblocks\descent\gi-p1\kronos\run-bake.ps1' -Tag "w2-$($b[0])" -Only $b[1] -TimeoutMin 10
        "bake $($b[0]) done $(Get-Content D:\Jacob\tblocks\descent\gi-p1\kronos\w2-$($b[0]).summary.txt -TotalCount 2 | Select-Object -Last 1)" | Add-Content "$o\slot_summary.txt"
        if (git -C $g status --porcelain --untracked-files=no) { throw "wt-gpu dirty after bake $($b[0]): STOP" }
    }
    # 2. main tour, mirror metallic 1.0
    $txt = [System.IO.File]::ReadAllText($mat)
    if (-not $txt.Contains('"_Metallic": 0.0,')) { throw 'M_Mirror is not matte as expected: STOP' }
    [System.IO.File]::WriteAllText($mat, $txt.Replace('"_Metallic": 0.0,', '"_Metallic": 1.0,'), (New-Object System.Text.UTF8Encoding($false)))
    $env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
    New-Item -ItemType Directory -Force "$o\main\probes" | Out-Null
    $env:TARTARUS_PROBE_LOG = '1'; $env:TARTARUS_PROBE_DUMP = "$o\main\probes"
    Run-Tour "$s\tour_main.json" "$o\main" 10
    Select-String -Path "$o\main\stdout.txt", "$o\main\stderr.txt" -Pattern 'ProbeCapture slot' | ForEach-Object { $_.Line } | Set-Content "$o\main\probe_log.txt"
    Remove-Item Env:TARTARUS_PROBE_LOG, Env:TARTARUS_PROBE_DUMP -ErrorAction SilentlyContinue
    git -C $g checkout -- project/assets/Environments/SuburbNeighborhoodHousePack/Materials/M_Mirror.mat
    # 3. moon A/B
    foreach ($cap in 0, 1, 3) {
        $env:TARTARUS_VOL_INSCATTER_CAP = "$cap"
        Run-Tour "$s\tour_moon.json" "$o\moon_cap$cap" 6
    }
    Remove-Item Env:TARTARUS_VOL_INSCATTER_CAP -ErrorAction SilentlyContinue
} finally {
    git -C $g checkout -- project/assets/Environments/SuburbNeighborhoodHousePack/Materials/M_Mirror.mat 2>$null
    git -C $g checkout -- project/scenes/Forest.gi 2>$null
    "wt-gpu status after: $(if (git -C $g status --porcelain --untracked-files=no) { 'DIRTY' } else { 'clean' }); end $((Get-Date).ToUniversalTime().ToString('o'))" | Add-Content "$o\slot_summary.txt"
}
Get-Content "$o\slot_summary.txt"
