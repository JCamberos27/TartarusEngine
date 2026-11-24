# Red-room metals A/B (Jacob: armchair rail grey-green / floor-lamp pole + table-lamp base near-black with hard streaks). Caller holds locks\perf (owner W2) after the SM's GO.
# Swaps the BUILD copies of ModelFragment.glsl and ModelShading.glsl per variant (hash-checked restore in finally). Tour: tour_redroom_metals.json (4 metals poses + sink).
#   base      = as built (main)
#   face1     = roomFace forced 1 (the room cube's specular/diffuse gate off): tests "metal faces away from the probe centre -> takes the SKY cube (grey-green)"
#   dspec0    = direct (lamp) specular off in ShadeLight: tests the hard bright streaks
#   dspec50   = direct specular x0.5
#   fix6      = dev/argus-spec-fix shader with kRoomSpecFloor 0.6
#   fix6face1 = fix6 + roomFace forced 1
param([string[]]$Variants = @('base','face1','dspec0','dspec50','fix6','fix6face1'),
      [string]$Gpu = 'D:\Jacob\tblocks\descent\wt-gpu',
      [string]$Fix = 'D:\Jacob\tblocks\descent\wt-argus-spec\src\Renderer\shaders\ModelFragment.glsl',
      [string]$Tour = 'D:\Jacob\tblocks\descent\wt-argus-spec\tools\descent\argus\tour_redroom_metals.json',
      [string]$Out = 'D:\Jacob\tblocks\descent\gi-p2\argus\red_ab')
$ErrorActionPreference = 'Stop'
$exe = "$Gpu\build\Release\TartarusEngine.exe"; $dir = "$Gpu\build\Release\assets\shaders"
$files = @('ModelFragment.glsl', 'ModelShading.glsl')
if (-not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W2' -Quiet)) { throw 'perf not W2: STOP' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'engine running: STOP' }
$utf8 = New-Object Text.UTF8Encoding($false)
$orig = @{}; $h0 = @{}
foreach ($f in $files) { $orig[$f] = [IO.File]::ReadAllText("$dir\$f"); $h0[$f] = (Get-FileHash "$dir\$f" -Algorithm SHA256).Hash; "before $f $($h0[$f])" }
$fixT = [IO.File]::ReadAllText($Fix)
function Swap($t, $a, $b) { if (-not $t.Contains($a)) { throw "text missing: $a" }; $t.Replace($a, $b) }
$rf = 'roomFace = smoothstep(-0.15, 0.05, dot(N, toCentre) / max(length(toCentre), 1e-3));'
$sp = 'vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4);'
try {
    foreach ($v in $Variants) {
        $frag = $orig['ModelFragment.glsl']; $shad = $orig['ModelShading.glsl']
        switch ($v) {
            'base'      { }
            'face1'     { $frag = Swap $frag $rf 'roomFace = 1.0;' }
            'dspec0'    { $shad = Swap $shad $sp ($sp + ' specular *= 0.0;') }
            'dspec50'   { $shad = Swap $shad $sp ($sp + ' specular *= 0.5;') }
            'fix6'      { $frag = Swap $fixT 'const float kRoomSpecFloor = 0.4;' 'const float kRoomSpecFloor = 0.6;' }
            'fix6face1' { $frag = Swap (Swap $fixT 'const float kRoomSpecFloor = 0.4;' 'const float kRoomSpecFloor = 0.6;') $rf 'roomFace = 1.0;' }
            default     { throw "unknown variant $v" }
        }
        [IO.File]::WriteAllText("$dir\ModelFragment.glsl", $frag, $utf8); [IO.File]::WriteAllText("$dir\ModelShading.glsl", $shad, $utf8)
        $o = "$Out\$v"; New-Item -ItemType Directory -Force $o | Out-Null
        $env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
        $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$o\stdout.txt" -RedirectStandardError "$o\stderr.txt" `
            -ArgumentList '--project', "$Gpu\project", '--gi-tour', $Tour, '--gi-tour-view', 'full', '--out', "$o\png", '--perf-res', '1920x1080'
        $null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
        if (-not $p.WaitForExit(240000)) { Stop-Process -Id $p.Id -Force; "$v timeout-killed" } else { "$v exit $($p.ExitCode)" }
    }
} finally {
    foreach ($f in $files) { [IO.File]::WriteAllText("$dir\$f", $orig[$f], $utf8); $h1 = (Get-FileHash "$dir\$f" -Algorithm SHA256).Hash; "after  $f $h1 $(if ($h1 -eq $h0[$f]) { 'RESTORED' } else { 'DIFFERS: STOP' })" }
    "wt-gpu: $(if (git -C $Gpu status --porcelain --untracked-files=no) { 'DIRTY' } else { 'clean' })"
}
Get-Date -Format u
