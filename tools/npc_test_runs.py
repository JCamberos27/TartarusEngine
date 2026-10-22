# Runs NPC test scenarios several times each and reports every check's pass rate: one run of a test whose
# fight plays out differently each time (aptfight) can't tell a fix from noise.
#   python tools/npc_test_runs.py <engine exe> <scenario[@room]>... [--runs N] [--parallel N] [--logs DIR] [--visible]
# A scenario may carry a room for aptwounds (NPC_TEST_ROOM), e.g. aptwounds@303bed2. Runs go hidden and muted
# unless --visible (then muted only). At most 4 engines at once (VRAM). Exit code 1 if any check ever failed.
import argparse
import os
import re
import subprocess
import sys
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor

LINE = re.compile(r'^\[NpcTest\] (PASS|FAIL) (.*)$')
SUMMARY = re.compile(r'^\[SmokeTest\] (PASS|FAIL)  .*?(?:cause: (.*))?$')


def check_name(text):
    # The check without its measurement: "no pose pop over 20 cm in a frame (worst 19.4 cm)" -> "... in a frame".
    return re.sub(r'\s*\([^()]*\)\s*$', '', text).strip()


def run_one(exe, workdir, scenario, room, log, visible):
    env = dict(os.environ, TARTARUS_MUTE='1')
    if visible:
        env.pop('TARTARUS_HIDDEN', None)
    else:
        env['TARTARUS_HIDDEN'] = '1'
    if room:
        env['NPC_TEST_ROOM'] = room
    with open(log, 'w', encoding='utf-8', errors='replace') as out:
        subprocess.run([exe, '--npc-test', scenario], cwd=workdir, env=env, stdout=out, stderr=subprocess.STDOUT)
    checks, finished = OrderedDict(), None
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip()
            m = LINE.match(line)
            if m:
                checks[check_name(m.group(2))] = (m.group(1) == 'PASS', m.group(2))
            m = SUMMARY.match(line)
            if m and finished is None:
                finished = "didn't finish" not in (m.group(2) or '')
    return checks, finished


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('scenarios', nargs='+')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--parallel', type=int, default=4)
    ap.add_argument('--logs', default='npc_runs')
    ap.add_argument('--visible', action='store_true')
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    workdir = os.path.abspath(os.path.join(os.path.dirname(exe), '..', '..'))  # <repo>/b/Release/TartarusEngine.exe
    os.makedirs(a.logs, exist_ok=True)
    jobs = []
    for spec in a.scenarios:
        scenario, _, room = spec.partition('@')
        for k in range(a.runs):
            jobs.append((spec, scenario, room, os.path.join(a.logs, f"{spec.replace('@', '_')}_{k + 1}.log")))
    with ThreadPoolExecutor(max_workers=max(1, min(a.parallel, 4))) as pool:
        results = list(pool.map(lambda j: (j[0], j[3], run_one(exe, workdir, j[1], j[2], j[3], a.visible)), jobs))

    failed = False
    for spec in a.scenarios:
        mine = [(log, r) for s, log, r in results if s == spec]
        print(f'== {spec}: {len(mine)} run(s)')
        names = OrderedDict()
        for _, (checks, _) in mine:
            for n in checks:
                names[n] = True
        for n in names:
            seen = [checks[n] for _, (checks, _) in mine if n in checks]
            passed = sum(1 for ok, _ in seen if ok)
            mark = 'ok  ' if passed == len(seen) == len(mine) else 'FLAKY' if passed else 'FAIL'
            failed = failed or passed < len(seen)
            print(f'  {mark} {passed}/{len(seen)}' + (f' (in {len(seen)} of {len(mine)} runs)' if len(seen) < len(mine) else '') + f'  {n}')
            for ok, text in seen:
                if not ok:
                    print(f'         {text}')
        unfinished = [log for log, (_, fin) in mine if not fin]
        if unfinished:
            failed = True
            print(f"  FAIL didn't finish: {', '.join(os.path.basename(u) for u in unfinished)}")
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
