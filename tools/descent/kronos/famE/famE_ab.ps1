# W5: perf A/B for Atlas's family E (SM order; GPU only on "GPU GO W5", after Jacob's look). W5 holds locks\perf.
#  A = wt-gpu's Forest.json as checked out (677c753d at prep time).
#  B = A + ONLY family E from dev/fix3-jambs ced5d6b7: added unshadowed fills 38103/38104/38105 (strip x 13.42..14.58,
#      z 24.04..31.66) and the trimmed originals 36392/36394/35609 (+x 13.61). The jamb Bounds edits on that branch are left
#      out, and the exe is the same, so the only difference is family E.
#  Tour: exterior/facade looks (jlook-facade-*, porch, bay-east, jamb-35614_east) + the living-room sweep views (the strip's
#  room). Runs INTERLEAVED A, B, A, B (-Repeats rounds) to cancel warm-up/order effects. Scene copies are untracked in
#  wt-gpu\project\scenes and are deleted at the end. Analysis: famE_ab.py -> famE\ab.md.
param([int]$Repeats = 2, [int]$TimeoutMin = 10)
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'
$exe = "$g\build\Release\TartarusEngine.exe"
$out = 'D:\Jacob\tblocks\descent\gi-p2\kronos\famE'
if (-not ((Get-Content 'D:\Jacob\tblocks\descent\locks\perf\owner.txt') -match 'W5')) { throw 'locks\perf not held by W5' }
if (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue).Count) { throw 'a TartarusEngine is running: no GPU' }
if (git -C $g status --porcelain --untracked-files=no) { throw 'wt-gpu has tracked changes' }
$log = "$out\ab_summary.txt"
"famE A/B: wt-gpu $(git -C $g rev-parse --short HEAD), exe $((Get-Item $exe).LastWriteTime); B = A + family E from ced5d6b7; NOT a gated perf number" | Set-Content $log
& git -C $g show ced5d6b7:project/scenes/Forest.json | Set-Content -Encoding utf8 "$out\Forest_ced5d6b7.json"
& python -I "$out\famE_make.py" "$g\project\scenes\Forest.json" "$out\Forest_ced5d6b7.json" "$g\project\scenes" 'D:\Jacob\tblocks\descent\house-polish\briareos\sweep\sweep_tour_open.json' $out | Add-Content $log
if ($LASTEXITCODE) { throw 'scene/tour build failed' }
$env:TARTARUS_HIDDEN = '1'; $env:TARTARUS_MUTE = '1'
try {
    for ($r = 1; $r -le $Repeats; $r++) {
        foreach ($arm in 'A', 'B') {
            $tag = "${arm}$r"
            $p = Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru `
                -ArgumentList '--project', "$g\project", '--gi-tour', "$out\tour_$arm.json", '--gi-tour-view', 'full', '--out', "$out\$tag", '--perf-res', '1920x1080' `
                -RedirectStandardOutput "$out\${tag}_run.log" -RedirectStandardError "$out\${tag}_run.log.err"
            $null = $p.Handle
            try { $p.PriorityClass = 'BelowNormal' } catch {}
            if (-not $p.WaitForExit($TimeoutMin * 60000)) { Stop-Process -Id $p.Id -Force; $res = 'timeout-killed' }
            else { $res = (Select-String -Path "$out\${tag}_run.log" -Pattern '\[SmokeTest\] (PASS|FAIL)' | Select-Object -Last 1).Line }
            "${tag}: exit=$($p.ExitCode) | $res" | Add-Content $log
        }
    }
} finally {
    Remove-Item "$g\project\scenes\Forest_famE_A.json", "$g\project\scenes\Forest_famE_B.json", "$g\project\scenes\Forest_famE_C.json" -ErrorAction SilentlyContinue
    "scene copies removed; wt-gpu tracked changes: $(if (git -C $g status --porcelain --untracked-files=no) { 'YES' } else { 'none' })" | Add-Content $log
}
& python -I "$out\famE_ab.py" $out $Repeats | Out-File "$out\ab.md" -Encoding utf8
Get-Content $log
