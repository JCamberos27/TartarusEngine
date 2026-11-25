#!/usr/bin/env bash
# G-MB: is the forest-border-2 fine stipple (LOD cross-fade caught mid-fade) run-to-run in OFF too? Border 1 -> 2 only,
# OFF/ON alternating x4, untimed under the perf lock; then the 1px/2px detail ratio per run (> 0.73 = mid-fade stipple).
set -u
cd /c/Users/jacob/tbw1c || exit 4
L=verify/gmb_fade; mkdir -p $L
while [ -d /c/Users/jacob/tblocks/quiet ]; do sleep 20; done
until mkdir /c/Users/jacob/tblocks/perf 2>/dev/null; do sleep 20; done
echo "W1 $(date -u +%Y-%m-%dT%H:%M:%SZ) G-MB fade check (untimed)" > /c/Users/jacob/tblocks/perf/owner.txt
trap 'rm -f /c/Users/jacob/tblocks/perf/owner.txt; rmdir /c/Users/jacob/tblocks/perf' EXIT
export TARTARUS_HIDDEN=1 TARTARUS_MUTE=1 TARTARUS_EDITOR_TEST_INSTANCE=w1c TARTARUS_SHOT_SIZE=1920x1080
CAMS="border1:3,1.7,12,90,0;border2:-14,1.8,17,40,-3"
for rep in 1 2 3 4; do
  for mode in off on; do
    d=$L/${mode}$rep; rm -rf $d; mkdir -p $d
    if [ $mode = off ]; then X=TARTARUS_NO_SLOT_STATE=1; else X=X_GMB=1; fi
    env $X TARTARUS_SHOT_CAMERAS="$CAMS" ./b/Release/TartarusEngine.exe --smoke-test "$(cygpath -w project/scenes/Forest.json)" \
      --smoke-shots "$(cygpath -w $d)" > $d/run.log 2>&1
    echo "$(date -u +%H:%M:%SZ) $mode$rep rc=$?"
  done
done
python - <<'EOF'
import glob, numpy as np
from PIL import Image
for p in sorted(glob.glob('verify/gmb_fade/*/Forest_border2.png')):
    a = np.asarray(Image.open(p).convert('L'), np.float32)[210:570, 0:640]
    r = np.abs(a[:, 1:] - a[:, :-1]).mean() / np.abs(a[:, 2:] - a[:, :-2]).mean()
    print(p.split('/')[-2].split(chr(92))[-1], f'{r:.3f}', 'STIPPLE' if r > 0.73 else 'blocky')
EOF
