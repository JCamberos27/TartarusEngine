# W2 moon-glow A/B #2 (needs locks\perf owned by W2 + a GPU GO; ~2.5 min). The first A/B (caps 0/1/2/3) was INVALID: the cap starts at cap/2 in HDR
# (pre-exposure) units and the scene has exposureEV +1.8 (x3.5), so a wedge that clips to white after exposure is only ~0.3 HDR, below the knee of
# caps >= 1: no effect was expected. Caps 0.15 / 0.3 / 0.5 start at 0.075 / 0.15 / 0.25 HDR. cap 0 = off (reference).
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'; $exe = "$g\build\Release\TartarusEngine.exe"; $s = 'D:\Jacob\tblocks\descent\gi-p2\argus\slot'; $o = "$s\out\moon2"
if (-not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W2' -Quiet)) { throw 'perf not W2' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'engine running' }
$env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
try {
    foreach ($cap in '0', '0.15', '0.3', '0.5') {
        $env:TARTARUS_VOL_INSCATTER_CAP = $cap
        $dir = "$o\cap$cap"; New-Item -ItemType Directory -Force $dir | Out-Null
        $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -ArgumentList '--project', "$g\project", '--gi-tour', "$s\tour_moon.json", '--gi-tour-view', 'full', '--out', "$dir\png", '--perf-res', '1920x1080' -RedirectStandardOutput "$dir\stdout.txt" -RedirectStandardError "$dir\stderr.txt"
        $null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
        if (-not $p.WaitForExit(240000)) { Stop-Process -Id $p.Id -Force }
        "cap $cap exit $($p.ExitCode)"
    }
} finally { Remove-Item Env:TARTARUS_VOL_INSCATTER_CAP -ErrorAction SilentlyContinue; "wt-gpu: $(if (git -C $g status --porcelain --untracked-files=no) { 'DIRTY' } else { 'clean' })" }
