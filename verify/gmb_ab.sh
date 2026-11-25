#!/usr/bin/env bash
# G-MB stage A: build, unit + smoke_min, GPU-cull verify (Forest/Hallway), then pixel A/B shots
# OFF (TARTARUS_NO_SLOT_STATE) / LEAD (TARTARUS_SLOT_STATE_LEAD) / ON, Forest + Hallway, untimed under the perf lock.
# Usage: bash verify/gmb_ab.sh [skipbuild]
set -u
cd /c/Users/jacob/tbw1c || exit 4
L=verify/gmb${GMB_TAG:-}
mkdir -p $L
T0=$(date -u +%s)
stamp() { echo "$(date -u +%H:%M:%SZ) $*" | tee -a $L/summary.txt; }
while [ -d /c/Users/jacob/tblocks/quiet ]; do echo "quiet: $(cat /c/Users/jacob/tblocks/quiet/owner.txt 2>/dev/null)"; sleep 20; done
if [ "${1:-}" != skipbuild ]; then
  B0=$(date -u +%s)
  cmake --build b --config Release --parallel 12 > $L/build.log 2>&1 || { stamp "BUILD FAILED"; grep -E "error|Error" $L/build.log | head -30; exit 6; }
  stamp "built in $(( $(date -u +%s) - B0 )) s"
fi
while [ -d /c/Users/jacob/tblocks/quiet ]; do sleep 20; done
until mkdir /c/Users/jacob/tblocks/perf 2>/dev/null; do echo "perf lock held: $(cat /c/Users/jacob/tblocks/perf/owner.txt 2>/dev/null)"; sleep 20; done
echo "W1 $(date -u +%Y-%m-%dT%H:%M:%SZ) G-MB stage A verify + pixel A/B (untimed)" > /c/Users/jacob/tblocks/perf/owner.txt
trap 'rm -f /c/Users/jacob/tblocks/perf/owner.txt; rmdir /c/Users/jacob/tblocks/perf' EXIT
export TARTARUS_HIDDEN=1 TARTARUS_MUTE=1 TARTARUS_EDITOR_TEST_INSTANCE=w1c
EXE=./b/Release/TartarusEngine.exe

$EXE --unit-tests > $L/unit.log 2>&1; stamp "unit rc=$? $(grep -iE 'passed|failed' $L/unit.log | tail -1)"
$EXE --smoke-test "$(cygpath -w tests/smoke-scenes/smoke_min.json)" > $L/smoke_min.log 2>&1; stamp "smoke_min rc=$?"

FOREST="project/scenes/Forest.json"; HALL="tests/scenes/Apartment Hallway.json"
TARTARUS_GPU_CULL_VERIFY=1 $EXE --perf-bench "$FOREST" --perf-res 1920x1080 > $L/verify_forest.log 2>&1; stamp "verify forest rc=$?"
TARTARUS_GPU_CULL_VERIFY=1 $EXE --perf-bench "$HALL" --perf-res 1920x1080 > $L/verify_hallway.log 2>&1; stamp "verify hallway rc=$?"

# Shots: yaw 90 = +z. Forest: the clearing -> house probes (clearing 0,4,6 imp 1; house ground 0,1.7,26.4 imp 2) and
# the house side; Hallway has no probes or GI volumes (control: must be 0 px).
export TARTARUS_SHOT_SIZE=1920x1080
FCAMS="border1:3,1.7,12,90,0;border2:-14,1.8,17,40,-3;clearing:0,2.5,-6,90,-6;house:6,1.6,22,150,-5"
HCAMS="hall1:2,1.6,0,0,0;hall2:19,1.6,0,180,0"
shots() { # mode scene cams tag
  local d=$L/shots_$1_$4; rm -rf $d; mkdir -p $d
  env $2 TARTARUS_SHOT_CAMERAS="$5" $EXE --smoke-test "$(cygpath -w "$3")" --smoke-shots "$(cygpath -w $d)" > $d/run.log 2>&1
  stamp "shots $1 $4 rc=$? $(ls $d/*.png 2>/dev/null | wc -l) png"
}
for rep in 1 2; do
  shots off$rep "TARTARUS_NO_SLOT_STATE=1" "$FOREST" forest "$FCAMS"
  shots lead$rep "TARTARUS_SLOT_STATE_LEAD=1" "$FOREST" forest "$FCAMS"
  shots on$rep "X_GMB=1" "$FOREST" forest "$FCAMS"
done
for rep in 1 2; do
  shots off$rep "TARTARUS_NO_SLOT_STATE=1" "$HALL" hallway "$HCAMS"
  shots lead$rep "TARTARUS_SLOT_STATE_LEAD=1" "$HALL" hallway "$HCAMS"
  shots on$rep "X_GMB=1" "$HALL" hallway "$HCAMS"
done
grep -h "GL_INVALID\|FAIL" $L/shots_*/run.log | sort | uniq -c | head -20 | tee -a $L/summary.txt
python verify/tools/gmb_pixdiff.py $L | tee -a $L/summary.txt
stamp "DONE total $(( $(date -u +%s) - T0 )) s"
