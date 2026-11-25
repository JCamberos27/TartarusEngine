#!/usr/bin/env bash
# G-MB fade check 2: border 2 captured three times, 30 frames apart (border2, border2b, border2c: the same pose).
# If the ON stipple is a LOD cross-fade still running at the first capture, the later captures are blocky.
set -u
cd /c/Users/jacob/tbw1c || exit 4
L=verify/gmb_fade2; mkdir -p $L
while [ -d /c/Users/jacob/tblocks/quiet ]; do sleep 20; done
until mkdir /c/Users/jacob/tblocks/perf 2>/dev/null; do sleep 20; done
echo "W1 $(date -u +%Y-%m-%dT%H:%M:%SZ) G-MB fade check 2 (untimed)" > /c/Users/jacob/tblocks/perf/owner.txt
trap 'rm -f /c/Users/jacob/tblocks/perf/owner.txt; rmdir /c/Users/jacob/tblocks/perf' EXIT
export TARTARUS_HIDDEN=1 TARTARUS_MUTE=1 TARTARUS_EDITOR_TEST_INSTANCE=w1c TARTARUS_SHOT_SIZE=1920x1080
B2="-14,1.8,17,40,-3"
CAMS="border1:3,1.7,12,90,0;border2:$B2;border2b:$B2;border2c:$B2"
for rep in 1 2 3; do
  for mode in off on; do
    d=$L/${mode}$rep; rm -rf $d; mkdir -p $d
    if [ $mode = off ]; then X=TARTARUS_NO_SLOT_STATE=1; else X=X_GMB=1; fi
    env $X TARTARUS_SHOT_CAMERAS="$CAMS" ./b/Release/TartarusEngine.exe --smoke-test "$(cygpath -w project/scenes/Forest.json)" \
      --smoke-shots "$(cygpath -w $d)" > $d/run.log 2>&1
    echo "$(date -u +%H:%M:%SZ) $mode$rep rc=$?"
  done
done
python verify/tools/gmb_fade_ratio.py $L
