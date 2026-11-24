# W2 debug slot runner (needs locks\perf owned by W2 + the SM's GPU GO). One run = tour_dbg.json (mirror head-on + kitchen sink basin) with
# M_Mirror _Metallic 1.0 temporarily and the BUILD's copy of ModelFragment.glsl edited, both restored in finally.
#   -Snippet : GLSL appended after the ambient line (e.g. 'ambient = vec3(roomSpecW, specIntensity * 12.5, 0.0);')
#   -From/-To: optional exact text replacement in the shader before the run (e.g. force roomFace = 1)
param([string]$Tag, [string]$Snippet = '', [string]$From = '', [string]$To = '')
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'; $exe = "$g\build\Release\TartarusEngine.exe"; $o = "D:\Jacob\tblocks\descent\gi-p2\argus\slot\out\dbg_$Tag"
if (-not (Select-String -Path 'D:\Jacob\tblocks\descent\locks\perf\owner.txt' -Pattern 'W2' -Quiet)) { throw 'perf not W2' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'engine running' }
$mat = "$g\project\assets\Environments\SuburbNeighborhoodHousePack\Materials\M_Mirror.mat"
$sh = "$g\build\Release\assets\shaders\ModelFragment.glsl"
$bak = "$sh.w2bak"
try {
    $t = [IO.File]::ReadAllText($mat)
    [IO.File]::WriteAllText($mat, $t.Replace('"_Metallic": 0.0,', '"_Metallic": 1.0,'), (New-Object Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath $sh -Destination $bak -Force
    $s = [IO.File]::ReadAllText($sh)
    if ($From) { if (-not $s.Contains($From)) { throw 'From text missing' }; $s = $s.Replace($From, $To) }
    if ($Snippet) {
        $k = 'ambient = (kD * diffuseIBL + specularIBL * specIntensity) * ao;'
        if (-not $s.Contains($k)) { throw 'anchor missing' }
        $s = $s.Replace($k, $k + "`r`n        " + $Snippet)
    }
    [IO.File]::WriteAllText($sh, $s, (New-Object Text.UTF8Encoding($false)))
    New-Item -ItemType Directory -Force $o | Out-Null
    $env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
    $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -ArgumentList '--project', "$g\project", '--gi-tour', 'D:\Jacob\tblocks\descent\gi-p2\argus\slot\tour_dbg.json', '--gi-tour-view', 'full', '--out', "$o\png", '--perf-res', '1920x1080' -RedirectStandardOutput "$o\stdout.txt" -RedirectStandardError "$o\stderr.txt"
    $null = $p.Handle; try { $p.PriorityClass = 'BelowNormal' } catch {}
    if (-not $p.WaitForExit(240000)) { Stop-Process -Id $p.Id -Force }
    "$Tag exit $($p.ExitCode)"
} finally {
    git -C $g checkout -- project/assets/Environments/SuburbNeighborhoodHousePack/Materials/M_Mirror.mat
    if (Test-Path -LiteralPath $bak) { Copy-Item -LiteralPath $bak -Destination $sh -Force; Remove-Item -LiteralPath $bak -Force }
    "wt-gpu: $(if (git -C $g status --porcelain --untracked-files=no) { 'DIRTY' } else { 'clean' }); shader restored"
}
