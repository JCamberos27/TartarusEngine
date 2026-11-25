#!/bin/bash
# Summary of a G2-b ON/OFF/ON set: machine line, frame pcts, key CPU/GPU scopes per phase.
D=/c/Users/jacob/tbw1c/verify/${1:-gmb_t1}
for r in f_on f_off f_on2 h_on h_off; do
  L=$D/$r.log; echo "===== $r"
  grep -h "view" $L | grep -i "perfbench\] view\|view mode" | head -3
  grep -h "^\[PerfBench\] .*\(frame ms\|machine\|Forest.json\)" $L | sed 's/\[PerfBench\] //'
  for ph in play play-max; do
    echo "-- $ph scopes"
    awk -v ph="($ph)" 'index($0,"" ph){on=1;next} on&&/^\[PerfBench\] [^ ]/{on=0} on' $L |
      grep -E "(CPU|GPU) (Scene: Opaque|Scene: Depth Pre-pass|Opaque|Depth Pre-pass|GPU Cull|Gather Visible|Scene Draw|Transform Cache|Asset Pump|Swap Buffers|Scene: Build Batches) " | sed 's/\[PerfBench\]  *//'
  done
done
grep -h "GpuCull\] verify" $D/f_on.log | tail -2
