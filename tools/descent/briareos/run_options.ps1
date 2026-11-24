# W6: OPTION before/after stills on wt-gpu (677c753d exe, scene swap only). Caller holds locks\perf. Each AFTER = the option's Forest.json patch applied to the tracked file, one engine run, then restored.
$ErrorActionPreference = 'Stop'
$g = 'D:\Jacob\tblocks\descent\wt-gpu'; $sw = 'D:\Jacob\tblocks\descent\house-polish\briareos\sweep'; $o = 'D:\Jacob\tblocks\descent\house-polish\sweep_fix3\options'
$run = "$sw\run_sweep.ps1"
& $run -Out "$o\asbuilt" -Tour "$sw\sweep_tour_opt_asbuilt.json" -TimeoutMin 10
foreach ($n in @(@('tableb', 'tableb_6c4b9d89'), @('south', 'south_525e3a75'), @('hallline', 'hallline_d26b5d3c'))) {
    try {
        git -C $g apply -p1 "$sw\optpatches\$($n[1]).patch"
        & $run -Out "$o\after_$($n[0])" -Tour "$sw\sweep_tour_opt_$($n[0]).json" -TimeoutMin 10 -SceneSwap
    } finally {
        git -C $g checkout -- project/scenes/Forest.json
    }
    "restored after $($n[0]): tracked changes = $((git -C $g status --porcelain --untracked-files=no | Measure-Object).Count), HEAD $(git -C $g rev-parse --short=8 HEAD)"
}
