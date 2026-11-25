#!/usr/bin/env bash
# G-MB part 1: build, then Forest/Hallway/Loco --perf-bench (edit + play phases) with the batch census on. Untimed, perf lock.
set -u
cd /c/Users/jacob/tbw1c || exit 4
[ -d /c/Users/jacob/tblocks/quiet ] && { echo "QUIET WINDOW ON - not running"; exit 5; }
cmake --build b --config Release --parallel 12 > verify/censusA_build.log 2>&1 || { echo "BUILD FAILED"; tail -30 verify/censusA_build.log; exit 6; }
echo "built $(date -u +%H:%M:%SZ)"
until mkdir /c/Users/jacob/tblocks/perf 2>/dev/null; do echo "perf lock held: $(cat /c/Users/jacob/tblocks/perf/owner.txt 2>/dev/null)"; sleep 20; done
echo "W1 $(date -u +%Y-%m-%dT%H:%M:%SZ) G-MB stage A census (untimed)" > /c/Users/jacob/tblocks/perf/owner.txt
trap 'rm -f /c/Users/jacob/tblocks/perf/owner.txt; rmdir /c/Users/jacob/tblocks/perf' EXIT
export TARTARUS_HIDDEN=1 TARTARUS_MUTE=1 TARTARUS_EDITOR_TEST_INSTANCE=w1 TARTARUS_BATCH_CENSUS=1
for s in forest hallway loco; do
  case $s in forest) sc="project/scenes/Forest.json";; hallway) sc="tests/scenes/Apartment Hallway.json";; loco) sc="tests/scenes/LocomotionLab.json";; esac
  ./b/Release/TartarusEngine.exe --perf-bench "$sc" --perf-res 1920x1080 > "verify/censusA_$s.log" 2>&1
  echo "$s exit $? $(date -u +%H:%M:%SZ)"
done
grep -h "\[BatchCensus\]\|\[PerfBench\] view (" verify/censusA_*.log > verify/censusA_summary.txt
echo DONE
