# Master-bedroom room probe A/B. Caller holds locks\perf (owner W2) after the SM's GO. Tour: tour_redroom_metals_w.json (warm-up shot, Jacob's camera, armchair rail, floor-lamp pole, table-lamp base, sink).
#   base      = scene + shaders as built
#   probe     = scene with 'Probe Master Bedroom' added (add_master_probe.py, ids 36200/268794) ; wt-gpu Forest.json restored with git checkout afterwards
#   imp05 / imp035 = probe at importance 0.5 / 0.35 (the leak fix); 'probe' = importance 3 (leaks into the kitchen)
#   probefix6 = probe + the dev/argus-spec-fix ModelFragment.glsl with kRoomSpecFloor 0.6
param([string[]]$Variants = @('imp05','imp035'),
      [string]$Gpu = 'D:\Jacob\tblocks\descent\wt-gpu',
      [string]$Fix = 'D:\Jacob\tblocks\descent\wt-argus-spec\src\Renderer\shaders\ModelFragment.glsl',
      [string]$Tool = 'D:\Jacob\tblocks\descent\wt-argus-spec\tools\descent\argus',
      [string]$Out = 'D:\Jacob\tblocks\descent\gi-p2\argus\probe_ab')
$ErrorActionPreference = 'Stop'
$exe = "$Gpu\build\Release\TartarusEngine.exe"; $sh = "$Gpu\build\Release\assets\shaders\ModelFragment.glsl"; $scene = "$Gpu\project\scenes\Forest.json"
if (-not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W2' -Quiet)) { throw 'perf not W2: STOP' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'engine running: STOP' }
if (git -C $Gpu status --porcelain --untracked-files=no) { throw 'wt-gpu not clean: STOP' }
$utf8 = New-Object Text.UTF8Encoding($false)
$origSh = [IO.File]::ReadAllText($sh); $hSh0 = (Get-FileHash $sh -Algorithm SHA256).Hash; "before shader $hSh0"
$fixT = [IO.File]::ReadAllText($Fix).Replace('const float kRoomSpecFloor = 0.4;', 'const float kRoomSpecFloor = 0.6;')
try {
    foreach ($v in $Variants) {
        git -C $Gpu checkout -- project/scenes/Forest.json
        [IO.File]::WriteAllText($sh, $origSh, $utf8)
        if ($v -ne 'base') { python "$Tool\add_master_probe.py" $scene --id 36200 --order 268794 }
        if ($v -eq 'probefix6') { [IO.File]::WriteAllText($sh, $fixT, $utf8) }
        $o = "$Out\$v"; New-Item -ItemType Directory -Force $o | Out-Null
        $env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
        $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$o\stdout.txt" -RedirectStandardError "$o\stderr.txt" `
            -ArgumentList '--project', "$Gpu\project", '--gi-tour', "$Tool\tour_redroom_metals_w.json", '--gi-tour-view', 'full', '--out', "$o\png", '--perf-res', '1920x1080'
        $null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
        if (-not $p.WaitForExit(240000)) { Stop-Process -Id $p.Id -Force; "$v timeout-killed" } else { "$v exit $($p.ExitCode)" }
    }
} finally {
    git -C $Gpu checkout -- project/scenes/Forest.json
    [IO.File]::WriteAllText($sh, $origSh, $utf8); $h1 = (Get-FileHash $sh -Algorithm SHA256).Hash
    "after shader $h1 $(if ($h1 -eq $hSh0) { 'RESTORED' } else { 'DIFFERS: STOP' })"
    "wt-gpu: $(if (git -C $Gpu status --porcelain --untracked-files=no) { 'DIRTY' } else { 'clean' })"
}
Get-Date -Format u
