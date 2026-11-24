# waits (max 25 min) until no foreign TartarusEngine is running, then runs the remaining option AFTER stills (south, hallline). Caller holds locks\perf.
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'; $sw = 'D:\Jacob\tblocks\descent\house-polish\briareos\sweep'; $o = 'D:\Jacob\tblocks\descent\house-polish\sweep_fix3\options'
$t0 = Get-Date
while (@(Get-Process TartarusEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -notlike "$g\*" }).Count -gt 0) {
    if (((Get-Date) - $t0).TotalMinutes -gt 25) { throw 'foreign TartarusEngine still running after 25 min' }
    Start-Sleep -Seconds 10
}
"foreign engine gone after $([int]((Get-Date) - $t0).TotalSeconds) s"
foreach ($n in @(@('south', 'south_525e3a75'), @('hallline', 'hallline_d26b5d3c'))) {
    try {
        git -C $g apply -p1 "$sw\optpatches\$($n[1]).patch"
        & "$sw\run_sweep.ps1" -Out "$o\after_$($n[0])" -Tour "$sw\sweep_tour_opt_$($n[0]).json" -TimeoutMin 10 -SceneSwap
    } finally { git -C $g checkout -- project/scenes/Forest.json }
    "restored after $($n[0]): tracked changes = $((git -C $g status --porcelain --untracked-files=no | Measure-Object).Count), HEAD $(git -C $g rev-parse --short=8 HEAD)"
}
