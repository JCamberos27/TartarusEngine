# The engine's `--model-report` (src/App/CliTools.cpp ModelReport), run and parsed in one place.
# One header line per model, then indented detail lines (tris, skin, bind) and a "N models, M failed" summary.
import os
import re
import subprocess

_HEAD = re.compile(r'\[ModelReport\] (.*) size (\S+) (\S+) (\S+) min (\S+) (\S+) (\S+) max (\S+) (\S+) (\S+) '
                   r'meshes (\d+) materials ?(.*)$')
_TRIS = re.compile(r'\[ModelReport\]\s+tris: (\d+)$')
_SUMMARY = re.compile(r'\[ModelReport\] \d+ models, (\d+) failed$')


def key(path):
    """The dict key for a model path: normcased and normalised, as every caller looks it up."""
    return os.path.normcase(os.path.normpath(path))


def parse(stdout):
    """-> ({key(path): {'size', 'min', 'max', 'meshes', 'materials', 'tris'}}, failed count or None if no summary)."""
    rows, cur, failed = {}, None, None
    for line in stdout.splitlines():
        line = line.strip()
        m = _HEAD.match(line)
        if m:
            v = [float(x) for x in m.groups()[1:10]]
            cur = rows[key(m.group(1))] = {'size': v[0:3], 'min': v[3:6], 'max': v[6:9], 'meshes': int(m.group(11)),
                                           'materials': [x for x in m.group(12).split('|') if x], 'tris': None}
            continue
        m = _TRIS.match(line)
        if m and cur is not None:
            cur['tris'] = int(m.group(1))
            continue
        m = _SUMMARY.match(line)
        if m:
            failed = int(m.group(1))
    return rows, failed


def run(exe, paths, budget=24000):
    """Run --model-report on model files and/or folders, in batches that keep each command line well under Windows'
    32767-character limit. -> (rows, failed (None if a run printed no summary), the last completed process)."""
    rows, failed, res = {}, 0, None
    batch, size = [], 0
    for p in list(paths) + [None]:
        if batch and (p is None or size + len(p) + 3 > budget):
            res = subprocess.run([exe, '--model-report', *batch], capture_output=True, text=True, encoding='utf-8',
                                 errors='replace')
            r, f = parse(res.stdout)
            rows.update(r)
            failed = None if failed is None or f is None else failed + f
            batch, size = [], 0
        if p is not None:
            batch.append(p)
            size += len(p) + 3
    return rows, failed, res
