"""Relaunch check for Rebuild & Restart's --restore-state (an editor-mode flag, so --editor-tests can't cover it).

Starts the editor the way a restart does, with a hand-over file whose scene lies OUTSIDE the project (so the usual
startup pick, EditorSettings::LastScenePath, never lands on it), and checks the editor reopened that scene, selected
the same objects and went back into Play. --restore-check keeps the run hidden in a fresh scratch user folder
(TartarusEngine-Restart-<instance>), so it never writes the user's own prefs, layout or log.

    python tools/restore_state_check.py build/Release/TartarusEngine.exe [--instance NAME] [--timeout SECONDS]

Exit code 0 when everything came back, 1 otherwise (each failed check is printed).
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Engine primitives only (CI has none of the external assets). Orders are what the hand-over selects by.
SCENE = {
    "formatVersion": 2,
    "_comment": "Rebuild & Restart relaunch check (tools/restore_state_check.py): a scene outside the project.",
    "assetFolders": [],
    "assetMeta": [],
    "boxes": [
        {"center": [0, -0.1, 0], "size": [12, 0.2, 12], "color": [0.34, 0.34, 0.37], "rotation": [0, 0, 0],
         "name": "Ground", "id": 0, "parentId": -1, "order": 0},
        {"center": [0, 0.5, 0], "size": [1, 1, 1], "color": [0.6, 0.4, 0.2], "rotation": [0, 0, 0],
         "name": "Crate", "id": 1, "parentId": -1, "order": 5},
    ],
    "empties": [
        {"name": "Sun", "id": 10, "parentId": -1, "order": 10, "position": [6, 10, 6], "rotation": [52, -36, 0],
         "scale": [1, 1, 1], "light": {"kind": "directional", "color": [1.0, 0.96, 0.9], "intensity": 4.0, "range": 30.0}},
    ],
    "models": [],
}
SELECTION = [{"order": 10, "name": "Sun"}, {"order": 5, "name": "Crate"}]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("exe", type=Path)
    ap.add_argument("--instance", default=os.environ.get("TARTARUS_EDITOR_TEST_INSTANCE") or "restorecheck")
    ap.add_argument("--timeout", type=float, default=240.0)
    args = ap.parse_args()
    if not args.exe.is_file():
        print(f"restore_state_check: no exe at {args.exe}")
        return 1

    work = Path(tempfile.mkdtemp(prefix="tartarus_restore_check_"))
    scene = work / "Restart Check.json"
    scene.write_text(json.dumps(SCENE, indent=2), encoding="utf-8")
    state = work / "restart_state.json"
    state.write_text(json.dumps({
        "version": 1,
        "scene": str(scene),
        "restoreRecovery": False,
        "wasPlaying": True,
        "selection": SELECTION,
        "savedAt": int(time.time()),
    }, indent=2), encoding="utf-8")
    report = work / "report.json"

    env = dict(os.environ, TARTARUS_EDITOR_TEST_INSTANCE=args.instance, TARTARUS_MUTE="1")
    cmd = [str(args.exe.resolve()), "--restore-state", str(state), "--restore-check", str(report)]
    print("restore_state_check: " + " ".join(cmd))
    try:
        rc = subprocess.run(cmd, env=env, cwd=str(args.exe.resolve().parent), timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
        print(f"restore_state_check: FAIL - the editor didn't quit within {args.timeout:.0f} s")
        return 1

    failures = []
    if rc != 0:
        failures.append(f"exit code {rc}")
    if state.exists():
        failures.append("the hand-over file is still there (ApplyRestartState deletes it)")
    if not report.is_file():
        failures.append("no report written")
    else:
        got = json.loads(report.read_text(encoding="utf-8"))
        print("restore_state_check: report " + json.dumps(got))
        if os.path.normcase(os.path.abspath(got.get("scene", ""))) != os.path.normcase(str(scene)):
            failures.append(f"scene {got.get('scene')!r}, want {str(scene)!r}")
        if not got.get("sceneLoaded"):
            failures.append("the scene didn't load")
        if sorted(got.get("selection", [])) != sorted(s["name"] for s in SELECTION):
            failures.append(f"selection {got.get('selection')}, want {[s['name'] for s in SELECTION]}")
        if not got.get("playing"):
            failures.append("not back in Play")
    for f in failures:
        print("restore_state_check: FAIL - " + f)
    if not failures:
        print("restore_state_check: PASS - outside-project scene reopened, selection and Play restored")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
