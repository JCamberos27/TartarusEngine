#!/bin/bash
# W2 Opaque per-feature split, untimed, Forest perf-bench on tbw2 (dev/audit2-w2-opaque-census DIAG). $1 = tag.
S=/c/Users/jacob/tbw2/scripts_w2/g1_one.sh; WT=/c/Users/jacob/tbw2; TAG=${1:-opcs1}
export G1_OUT=/c/Users/jacob/tbw2/g1ab/$TAG; mkdir -p $G1_OUT; O=C:/Users/jacob/tbw2/g1ab/$TAG
st() {
  local env="" unset=""
  for kv in ${2//,/ }; do [ -z "$kv" ] && continue; env="$env\$env:${kv%%=*}='${kv#*=}'; "; unset="$unset; Remove-Item Env:${kv%%=*}"; done
  echo "Step $1 -Log $O/$1.log { \$env:TARTARUS_EDITOR_TEST_INSTANCE='w2'; ${env}bash $S $1 $WT forest${unset} };"
}
CMD="$(st base TARTARUS_OPC=3)"
for b in 1 2 4 8 16 32 64; do CMD="$CMD $(st s$b TARTARUS_OPC=2,TARTARUS_OPC_STRIP=$b)"; done
powershell -NoProfile -ExecutionPolicy Bypass -File 'C:\Users\jacob\tblocks\tools\perf_run.ps1' -Worker w2 -Note "W2 Opaque split untimed ($TAG)" -WaitMinutes 60 -Cmd "$CMD"
echo "wrapper exit=$?"
