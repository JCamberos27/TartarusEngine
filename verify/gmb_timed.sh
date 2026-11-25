#!/bin/bash
# G-MB stage A timed Forest ON/OFF/ON (no shots): perf_run -Timed with -Log per step (view guard). Retries while the lock is held.
WT=/c/Users/jacob/tbw1c
TAG=${1:-gmb_t1}
OUT=$WT/verify/$TAG; W=C:/Users/jacob/tbw1c/verify/$TAG
export G1_OUT=$OUT
mkdir -p "$OUT"; cd "$WT" || exit 1
S=/c/Users/jacob/tbw2/scripts_w2/g1_one.sh
while :; do
  powershell -NoProfile -ExecutionPolicy Bypass -File 'C:\Users\jacob\tblocks\tools\perf_run.ps1' -Worker w1 -Timed -Note "W1 G-MB stage A ON/OFF/ON2 timed $TAG" -WaitMinutes 20 -Cmd "
Step warm -Log $W/warm.log { bash $S warm $WT forest };
Step f_on -Log $W/f_on.log { bash $S f_on $WT forest };
Step f_off -Log $W/f_off.log { \$env:TARTARUS_NO_SLOT_STATE='1'; bash $S f_off $WT forest; Remove-Item Env:TARTARUS_NO_SLOT_STATE };
Step f_on2 -Log $W/f_on2.log { bash $S f_on2 $WT forest };
Step h_on -Log $W/h_on.log { bash $S h_on $WT hallway };
Step h_off -Log $W/h_off.log { \$env:TARTARUS_NO_SLOT_STATE='1'; bash $S h_off $WT hallway; Remove-Item Env:TARTARUS_NO_SLOT_STATE }"
  rc=$?; [ $rc -ne 2 ] && break; sleep 30
done
echo "perf_run exit=$rc $(date -u +%H:%M:%SZ)"
for r in warm f_on f_off f_on2 h_on h_off; do echo "$r: $(grep -h -m2 -i 'view' $OUT/$r.log | head -2 | tr '\n' ' ')"; done | tee $OUT/viewmode.txt
