# Motion matching overhaul: turns a tools/mm_smoke.ps1 log folder into a comparable baseline (docs/mm_baseline/).
#
#   python tools/mm/baseline_summary.py <mm_smoke log dir> <out dir> [--traces <mm_smoke trace dir>]
#
# Writes to <out dir>:
#   logs/<run>.txt      each run's tagged lines ([NpcTest], [Slide], [Testbed], [PerfBench], ...), repo paths stripped
#   traces/<scenario>_trace.csv   the first run's NPC_TEST_TRACE trace.csv of every scenario that writes one
#   metrics.json        the numbers below, per run, for a script to compare against
#   SUMMARY.md          the same as tables: results and frame cost, foot slide, clip vs capsule speed, eye jitter, perf
# Later phases run it on their own mm_smoke output and compare with the committed baseline.
import argparse
import json
import os
import re
import shutil
import statistics
import sys
from collections import OrderedDict, defaultdict

TAGGED = re.compile(r'^\[[A-Z][A-Za-z]+\]')
RUN_NAME = re.compile(r'^npc_(?P<scenario>[a-z]+)(?:_(?P<run>\d+))?$')
SUMMARY = re.compile(r'^\[SmokeTest\] (?P<result>PASS|FAIL)  .+?  frames=\d+ rendered=(?P<rendered>\d+) wall=(?P<wall>[\d.]+)s '
                     r'fps=(?P<fps>\d+) unfocused=(?P<unfocused>\d+).*?(?:cause: (?P<cause>.*))?$')
CHECK = re.compile(r'^\[NpcTest\] (PASS|FAIL) (.*)$')
SLIDE = re.compile(r'^\[Slide\] (?P<phase>.+?)\s{2,}(?P<actor>player|Dummy \d+)\s+(?P<plants>\d+) plants, drift mean\s+(?P<mean>[\d.]+) cm, '
                   r'worst\s+(?P<worst>[\d.]+) cm')
SPEED = re.compile(r"^\[NpcTest\] (?:PASS|FAIL) (?P<leg>.+?): the gait loop's clips travel at the capsule's speed \(ratio (?P<ratio>[\d.]+) "
                   r'over (?P<frames>\d+) steady frames')
TOP_SPEED = re.compile(r'^\[NpcTest\] (?:PASS|FAIL) (?P<leg>.+?): top speed (?P<speed>[\d.]+) m/s')
JITTER = re.compile(r"^\[Testbed\] (?P<leg>.+?) eye jitter .*?camera off the body's eye rms (?P<cam_rms>[\d.]+) mm, max (?P<cam_max>[\d.]+) mm; "
                    r"body's eye off the capsule's rms (?P<eye_rms>[\d.]+) mm, max (?P<eye_max>[\d.]+) mm")
PERF_PHASE = re.compile(r'^\[PerfBench\] \S+ \((?P<phase>[\w-]+)\)\s+frames=(?P<frames>\d+)\s+avg=(?P<avg>[\d.]+) ms .*?worst=(?P<worst>[\d.]+) ms')
PERF_PCT = re.compile(r'^\[PerfBench\] (?P<phase>[\w-]+) frame ms: p50=(?P<p50>[\d.]+) p95=(?P<p95>[\d.]+) p99=(?P<p99>[\d.]+)')


def read_lines(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return [line.rstrip('\n') for line in f]


def strip_paths(line, repo):
    for form in (repo.replace('\\', '/'), repo.replace('/', '\\')):
        line = line.replace(form + '/', '').replace(form + '\\', '').replace(form, '<repo>')
    return line


def fmt(values, digits=2):
    if not values:
        return '-'
    if len(values) == 1:
        return f'{values[0]:.{digits}f}'
    return f'{statistics.mean(values):.{digits}f} ({min(values):.{digits}f}-{max(values):.{digits}f})'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs')
    ap.add_argument('out')
    ap.add_argument('--traces', default='')
    a = ap.parse_args()
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    os.makedirs(os.path.join(a.out, 'logs'), exist_ok=True)

    runs = OrderedDict()  # run name -> metrics
    for name in sorted(os.listdir(a.logs)):
        if not name.endswith('.log') or name.endswith('.stderr.log') or name == 'build.log':
            continue
        stem = name[:-4]
        lines = read_lines(os.path.join(a.logs, name))
        errors = []
        err_path = os.path.join(a.logs, stem + '.stderr.log')
        if os.path.exists(err_path):
            errors = sorted(set(strip_paths(l, repo) for l in read_lines(err_path) if l.startswith('[error]')))
        tagged = [strip_paths(l, repo) for l in lines if TAGGED.match(l) and not l.startswith('[WeaponAudio]')]
        with open(os.path.join(a.out, 'logs', stem + '.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(tagged + ([''] + ['stderr: ' + e for e in errors] if errors else [])) + '\n')

        m = OrderedDict(run=stem, errors=errors)
        rn = RUN_NAME.match(stem)
        m['scenario'] = rn.group('scenario') if rn else stem
        for l in lines:
            s = SUMMARY.match(l)
            if s and 'wall' not in m:
                m.update(result=s['result'], rendered=int(s['rendered']), wall_s=float(s['wall']), fps=int(s['fps']),
                         ms_per_frame=round(1000.0 * float(s['wall']) / max(1, int(s['rendered'])), 2),
                         unfocused=int(s['unfocused']), cause=(s['cause'] or '').strip())
        checks = [CHECK.match(l) for l in lines]
        checks = [c for c in checks if c]
        m['checks'] = len(checks)
        m['failed'] = [c.group(2) for c in checks if c.group(1) == 'FAIL']
        m['slide'] = [dict(phase=s['phase'].strip(), actor=s['actor'], plants=int(s['plants']), mean_cm=float(s['mean']),
                           worst_cm=float(s['worst'])) for s in map(SLIDE.match, lines) if s]
        m['clip_speed_ratio'] = [dict(leg=s['leg'], ratio=float(s['ratio']), frames=int(s['frames'])) for s in map(SPEED.match, lines) if s]
        m['top_speed'] = [dict(leg=s['leg'], speed=float(s['speed'])) for s in map(TOP_SPEED.match, lines) if s]
        m['eye_jitter'] = [dict(leg=s['leg'].strip(), cam_rms_mm=float(s['cam_rms']), cam_max_mm=float(s['cam_max']),
                                eye_rms_mm=float(s['eye_rms']), eye_max_mm=float(s['eye_max'])) for s in map(JITTER.match, lines) if s]
        perf = OrderedDict()
        for l in lines:
            p = PERF_PHASE.match(l)
            if p:
                perf.setdefault(p['phase'], {}).update(avg_ms=float(p['avg']), worst_ms=float(p['worst']), frames=int(p['frames']))
            p = PERF_PCT.match(l)
            if p:
                perf.setdefault(p['phase'], {}).update(p50_ms=float(p['p50']), p95_ms=float(p['p95']), p99_ms=float(p['p99']))
        if perf:
            m['perf'] = perf
        runs[stem] = m

    if a.traces and os.path.isdir(a.traces):
        os.makedirs(os.path.join(a.out, 'traces'), exist_ok=True)
        taken = set()
        for d in sorted(os.listdir(a.traces)):
            scenario = d.split('_')[0]
            src = os.path.join(a.traces, d, 'trace.csv')
            if scenario not in taken and os.path.exists(src):
                shutil.copyfile(src, os.path.join(a.out, 'traces', scenario + '_trace.csv'))
                taken.add(scenario)

    with open(os.path.join(a.out, 'metrics.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(list(runs.values()), f, indent=1)

    by_scenario = OrderedDict()
    for m in runs.values():
        by_scenario.setdefault(m['scenario'], []).append(m)

    out = ['# Motion matching baseline: summary', '',
           f'Generated by `tools/mm/baseline_summary.py` from {len(runs)} run(s). Values with several runs are '
           '"mean (min-max)".', '',
           '## Results and frame cost', '',
           '| Scenario | Runs | Checks passed | Failed checks | Log errors | ms / frame | fps | Unfocused frames |',
           '|---|---|---|---|---|---|---|---|']
    for scenario, ms in by_scenario.items():
        if 'wall_s' not in ms[0]:
            continue
        failed = sorted(set(f for m in ms for f in m['failed']))
        passed = [m['checks'] - len(m['failed']) for m in ms]
        errs = sorted(set(re.sub(r"'.*/", "'", e) for m in ms for e in m['errors']))
        out.append(f"| {scenario} | {len(ms)} | {min(passed)}-{max(passed)} of {ms[0]['checks']} | "
                   f"{'; '.join(failed) or '-'} | {'; '.join(errs) or '-'} | {fmt([m['ms_per_frame'] for m in ms])} | "
                   f"{fmt([float(m['fps']) for m in ms], 0)} | {', '.join(str(m['unfocused']) for m in ms)} |")
    out += ['', 'ms / frame is wall time over rendered frames from the scene load on (loading included), at unlocked FPS.',
            'Game time is a fixed 1/60 s step, so the checks do not depend on it.', '']

    slide = defaultdict(list)
    for scenario, ms in by_scenario.items():
        for m in ms:
            for s in m['slide']:
                slide[(scenario, s['phase'], s['actor'])].append(s)
    if slide:
        out += ['## Foot slide (planted-foot drift)', '',
                '| Scenario | Phase | Actor | Plants | Drift mean, cm | Drift worst, cm |', '|---|---|---|---|---|---|']
        for (scenario, phase, actor), ss in slide.items():
            out.append(f"| {scenario} | {phase} | {actor} | {fmt([float(s['plants']) for s in ss], 0)} | "
                       f"{fmt([s['mean_cm'] for s in ss], 1)} | {fmt([s['worst_cm'] for s in ss], 1)} |")
        out.append('')

    speed = defaultdict(list)
    for scenario, ms in by_scenario.items():
        for m in ms:
            for s in m['clip_speed_ratio']:
                speed[(scenario, s['leg'])].append(s['ratio'])
    top = defaultdict(list)
    for scenario, ms in by_scenario.items():
        for m in ms:
            for s in m['top_speed']:
                top[(scenario, s['leg'])].append(s['speed'])
    if speed or top:
        out += ['## Clip speed vs capsule speed', '', '| Scenario | Leg | Clip / capsule speed ratio |', '|---|---|---|']
        for (scenario, leg), v in speed.items():
            out.append(f'| {scenario} | {leg} | {fmt(v, 4)} |')
        if top:
            out += ['', '| Scenario | Leg | Dummy top speed, m/s |', '|---|---|---|']
            for (scenario, leg), v in top.items():
                out.append(f'| {scenario} | {leg} | {fmt(v, 3)} |')
        out.append('')

    jit = defaultdict(list)
    for scenario, ms in by_scenario.items():
        for m in ms:
            for j in m['eye_jitter']:
                jit[(scenario, j['leg'])].append(j)
    if jit:
        out += ['## Eye jitter (frame-to-frame second difference)', '',
                "| Scenario | Leg | Camera off body's eye rms / max, mm | Body's eye off capsule rms / max, mm |", '|---|---|---|---|']
        for (scenario, leg), js in jit.items():
            out.append(f"| {scenario} | {leg} | {fmt([j['cam_rms_mm'] for j in js], 3)} / {fmt([j['cam_max_mm'] for j in js], 3)} | "
                       f"{fmt([j['eye_rms_mm'] for j in js], 3)} / {fmt([j['eye_max_mm'] for j in js], 3)} |")
        out.append('')

    perf_runs = [m for m in runs.values() if 'perf' in m]
    if perf_runs:
        out += ['## Perf bench', '', '| Phase | avg ms | p50 ms | p95 ms | p99 ms | worst ms |', '|---|---|---|---|---|---|']
        phases = OrderedDict((p, None) for m in perf_runs for p in m['perf'])
        for p in phases:
            v = [m['perf'][p] for m in perf_runs if p in m['perf']]
            out.append(f"| {p} | {fmt([x.get('avg_ms', 0) for x in v])} | {fmt([x.get('p50_ms', 0) for x in v])} | "
                       f"{fmt([x.get('p95_ms', 0) for x in v])} | {fmt([x.get('p99_ms', 0) for x in v])} | "
                       f"{fmt([x.get('worst_ms', 0) for x in v])} |")
        out.append('')

    with open(os.path.join(a.out, 'SUMMARY.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print(f'{len(runs)} run(s) -> {a.out}')


if __name__ == '__main__':
    sys.exit(main())
