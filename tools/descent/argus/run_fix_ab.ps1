# Room-cube specular fix A/B (red-room metal too shiny + sink too dark). Caller holds locks\perf (owner W2) after the SM's GPU GO.
# Runs tour_redroom_sink.json once per variant; each variant = the BUILD copy of ModelFragment.glsl replaced (restored in finally).
#   base   = the shader as built (main)               -> the "before" stills
#   fix    = this branch's ModelFragment.glsl (clamp 4, floor 0.4, spec AO)
#   c2/c8  = fix with kRoomSpecClamp 2 / 8
#   f2/f6  = fix with kRoomSpecFloor 0.2 / 0.6
#   nospec = roomSpecW forced 0 (pre-spec-unscale x0.08): confirms the cause
# Output: $Out\<variant>\png. Paths: edit $Gpu / $Fix for the machine. Never run a second engine; wt-gpu must be tracked-clean.
param([string[]]$Variants = @('base','fix','c2','c8','f2','f6','nospec'),
      [string]$Gpu = 'D:\Jacob\tblocks\descent\wt-gpu',
      [string]$Fix = 'D:\Jacob\tblocks\descent\wt-argus-spec\src\Renderer\shaders\ModelFragment.glsl',
      [string]$Tour = 'D:\Jacob\tblocks\descent\wt-argus-spec\tools\descent\argus\tour_redroom_sink.json',
      [string]$Out = 'D:\Jacob\tblocks\descent\gi-p2\argus\fix_ab')
$ErrorActionPreference = 'Stop'
$exe = "$Gpu\build\Release\TartarusEngine.exe"; $sh = "$Gpu\build\Release\assets\shaders\ModelFragment.glsl"; $bak = "$sh.w2bak"
if (-not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W2' -Quiet)) { throw 'perf not W2: STOP' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'engine running: STOP' }
$utf8 = New-Object Text.UTF8Encoding($false)
$orig = [IO.File]::ReadAllText($sh); $fixT = [IO.File]::ReadAllText($Fix)
function Swap($t, $a, $b) { if (-not $t.Contains($a)) { throw "text missing: $a" }; $t.Replace($a, $b) }
Copy-Item -LiteralPath $sh -Destination $bak -Force
$h0 = (Get-FileHash -LiteralPath $sh -Algorithm SHA256).Hash
"build shader SHA256 before: $h0"
try {
    foreach ($v in $Variants) {
        switch ($v) {
            'base'   { $t = $orig }
            'fix'    { $t = $fixT }
            'c2'     { $t = Swap $fixT 'const float kRoomSpecClamp = 4.0;' 'const float kRoomSpecClamp = 2.0;' }
            'c8'     { $t = Swap $fixT 'const float kRoomSpecClamp = 4.0;' 'const float kRoomSpecClamp = 8.0;' }
            'f2'     { $t = Swap $fixT 'const float kRoomSpecFloor = 0.4;' 'const float kRoomSpecFloor = 0.2;' }
            'f6'     { $t = Swap $fixT 'const float kRoomSpecFloor = 0.4;' 'const float kRoomSpecFloor = 0.6;' }
            'nospec' { $t = Swap $orig 'const float specIntensity = mix(uIBLIntensity, 1.0, clamp(roomSpecW, 0.0, 1.0));' 'const float specIntensity = uIBLIntensity;' }
            default  { throw "unknown variant $v" }
        }
        [IO.File]::WriteAllText($sh, $t, $utf8)
        $o = "$Out\$v"; New-Item -ItemType Directory -Force $o | Out-Null
        $env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
        $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$o\stdout.txt" -RedirectStandardError "$o\stderr.txt" `
            -ArgumentList '--project', "$Gpu\project", '--gi-tour', $Tour, '--gi-tour-view', 'full', '--out', "$o\png", '--perf-res', '1920x1080'
        $null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
        if (-not $p.WaitForExit(240000)) { Stop-Process -Id $p.Id -Force; "$v timeout-killed" } else { "$v exit $($p.ExitCode)" }
        $glErr = Select-String -Path "$o\stdout.txt","$o\stderr.txt" -Pattern 'error|GLSL|compile' -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 3
        if ($glErr) { "  $v LOG: $($glErr.Line -join ' | ')" }
    }
} finally {
    Copy-Item -LiteralPath $bak -Destination $sh -Force; Remove-Item -LiteralPath $bak -Force
    $h1 = (Get-FileHash -LiteralPath $sh -Algorithm SHA256).Hash
    "build shader SHA256 after : $h1  $(if ($h0 -eq $h1) { '== before: RESTORED' } else { 'DIFFERS: STOP' })"
    $src = "$Gpu\src\Renderer\shaders\ModelFragment.glsl"
    if (Test-Path -LiteralPath $src) { "wt-gpu source shader SHA256: $((Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash)" }
    "shader restored; wt-gpu: $(if (git -C $Gpu status --porcelain --untracked-files=no) { 'DIRTY' } else { 'clean' })"
}
Get-Date -Format u
