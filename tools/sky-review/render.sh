#!/bin/sh
# Renders review screenshots of every scene in a folder with the current build, for comparing
# sky / cloud / lighting changes without driving the editor.
#
#   tools/sky-review/render.sh <out-dir> [scene-dir]
#
# scene-dir defaults to tools/sky-review/scenes. Each scene gives three Scene-view PNGs
# (<name>_0/_1/_2: toward the horizon, a higher angle the other way, and up at the sky) from a
# camera at (0, 2, 12). Shaders are copied from src/ first, so shader-only edits need no rebuild.
# Run from the repository root (Git Bash).
set -e
ROOT=$(pwd)
OUT=$1
SCENES=${2:-$ROOT/tools/sky-review/scenes}
[ -n "$OUT" ] || { echo "usage: $0 <out-dir> [scene-dir]"; exit 1; }
cp "$ROOT"/src/Renderer/shaders/*.glsl "$ROOT"/build/Release/assets/shaders/
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
cd "$ROOT/build/Release"
./TartarusEngine.exe --smoke-test "$SCENES" --smoke-shots "$OUT" > "$OUT/render.log" 2>&1 || true
grep -E "FAIL|ALL PASSED|rror" "$OUT/render.log" | tail -5
# The editor re-saves asset .meta files while it runs; undo that churn.
cd "$ROOT" && git checkout -q -- project
