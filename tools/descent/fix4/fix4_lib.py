"""Shared helpers for the fix-4 re-apply scripts (Talos).

Forest.json is patched TEXTUALLY (only the touched Light property lines change),
so the editor's float formatting and everything else stay byte-identical.
Entities are matched by NAME + POSITION + PARENT NAME (ids were renumbered by #1216).
A change is applied only if the current value equals the old value (apply) or the new
value (already applied, skipped). Anything else, a missing entity, or an ambiguous match
raises SystemExit with a loud message.
"""
import json
import os
import re
import sys

DEFAULT_SCENE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "project", "scenes", "Forest.json")
TOL = 1e-4
TOK = re.compile(r'-?\d[\d.eE+-]*|true|false|"[^"]*"')


def split_blocks(lines):
    """Return [(start_idx, end_idx_inclusive)] for every entity block of every top-level
    list (entities are '    {' ... '    }' / '    },' at indent 4)."""
    blocks = []
    start = None
    for i, ln in enumerate(lines):
        if ln == "    {":
            start = i
        elif ln in ("    }", "    },") and start is not None:
            blocks.append((start, i))
            start = None
    return blocks


def block_json(lines, s, e):
    txt = "\n".join(lines[s:e + 1]).rstrip(",")
    return json.loads(txt)


def load_entities(lines):
    ents = []
    for s, e in split_blocks(lines):
        d = block_json(lines, s, e)
        if "id" in d and "name" in d:
            ents.append((s, e, d))
    return ents


def close(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and not isinstance(b, bool):
        return abs(a - b) <= TOL
    return a == b


def find_prop(lines, s, e, key):
    """Locate the Light property `key` inside block lines[s..e].
    Returns (first_line, last_line) of the property text, or None."""
    in_light = False
    for i in range(s, e + 1):
        ln = lines[i]
        if ln == '      "Light": {':
            in_light = True
            continue
        if in_light and ln.startswith("      }"):
            return None
        if in_light and ln.startswith('        "%s": ' % key):
            if ln.rstrip().endswith("[") or ln.rstrip().endswith("{"):
                j = i + 1
                while not lines[j].startswith("        ]") and not lines[j].startswith("        }"):
                    j += 1
                return (i, j)
            return (i, i)
    return None


def light_keys_range(lines, s, e):
    """Return (first_key_line, closing_line) of the Light object."""
    a = None
    for i in range(s, len(lines)):
        if lines[i] == '      "Light": {':
            a = i + 1
        elif a is not None and lines[i].startswith("      }"):
            return a, i
    raise SystemExit("no Light block")


def render(key, val):
    """val: str token (scalar) or list of str tokens (array). Returns the lines (no trailing comma)."""
    if isinstance(val, list):
        out = ['        "%s": [' % key]
        for k, t in enumerate(val):
            out.append("          " + t + ("," if k < len(val) - 1 else ""))
        out.append("        ]")
        return out
    return ['        "%s": %s' % (key, val)]


def eq(cur, want, key):
    """Compare a current Light value with a wanted one; an absent 'Bounds Fade +/-' equals [-1,-1,-1]."""
    if key.startswith("Bounds Fade ") and key != "Bounds Fade":
        if cur is None:
            cur = [-1.0, -1.0, -1.0]
        if want is None:
            want = [-1.0, -1.0, -1.0]
    if cur is None or want is None:
        return cur is None and want is None
    return close(cur, want)


def tok_to_py(val):
    if isinstance(val, list):
        return [tok_to_py(v) for v in val]
    return json.loads(val)


def apply_changes(changes, scene=DEFAULT_SCENE, label="fix4", dry=False):
    scene = os.path.abspath(scene)
    with open(scene, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    nl = "\r\n" if "\r\n" in text[:5000] else "\n"
    lines = text.split(nl)
    ents = load_entities(lines)
    by_id = {d["id"]: d for _, _, d in ents}
    edits = []  # (start_line, end_line, new_lines) from the bottom up later
    report = []
    for ch in changes:
        cands = []
        for s, e, d in ents:
            if d["name"] != ch["name"]:
                continue
            if not close(d.get("position"), ch["position"]):
                continue
            pn = by_id.get(d.get("parentId"), {}).get("name") if d.get("parentId", -1) in by_id else None
            if pn != ch["parent"]:
                continue
            if "Light" not in d:
                continue
            cands.append((s, e, d))
        tag = "%s @ %s (parent %s)" % (ch["name"], ch["position"], ch["parent"])
        if len(cands) != 1:
            raise SystemExit("FAIL [%s] %s: %d matches (need exactly 1)" % (label, tag, len(cands)))
        s, e, d = cands[0]
        light = d["Light"]
        for key, spec in ch["props"].items():
            old_toks, new_tok = spec[0], spec[1]
            laters = [tok_to_py(t) for t in (spec[2] if len(spec) > 2 else [])]
            # old_toks: list of acceptable PRIOR values (history of this key on the source branch,
            # incl. intermediate commits that main may already carry); None = key absent.
            olds = [None if t is None else tok_to_py(t) for t in old_toks]
            new = None if new_tok is None else tok_to_py(new_tok)
            old = olds[0]
            cur = light.get(key)
            if eq(cur, new, key) or any(eq(cur, l, key) for l in laters):
                report.append("skip (already)   id %d  %s  %s" % (d["id"], tag, key))
                continue
            if not any(eq(cur, o, key) for o in olds):
                raise SystemExit("FAIL [%s] id %d %s: %s is %r, expected one of %r or new %r" % (label, d["id"], tag, key, cur, olds, new))
            loc = find_prop(lines, s, e, key)
            if new is None:
                if loc is None:
                    raise SystemExit("FAIL [%s] %s: cannot find %s to remove" % (label, tag, key))
                edits.append((loc[0], loc[1], None, key, d, tag, old, new))
            elif loc is not None:
                nt = new_tok
                if isinstance(new_tok, list):
                    ctoks = TOK.findall(" ".join(lines[loc[0]:loc[1] + 1]).split(": ", 1)[1])
                    if len(ctoks) == len(new_tok):
                        nt = [c if close(json.loads(c), json.loads(n)) else n for c, n in zip(ctoks, new_tok)]
                edits.append((loc[0], loc[1], render(key, nt), key, d, tag, old, new))
            else:
                # insert in the file's key order: before the first existing key that sorts after `key`
                keys = list(light.keys())
                after = [k for k in keys if k > key]
                if after:
                    nxt = find_prop(lines, s, e, after[0])
                    pos = nxt[0]
                else:
                    pos = light_keys_range(lines, s, e)[1]
                edits.append((pos, pos - 1, render(key, new_tok), key, d, tag, old, new))
            report.append("apply             id %d  %s  %s: %s -> %s" % (d["id"], tag, key, old, new))
    # comma fix-ups: a property line that is not the last in the Light block ends with ','
    # work bottom-up so line numbers stay valid
    for a, b, new_lines, key, d, tag, old, new in sorted(edits, key=lambda x: -x[0]):
        s, e = [(s_, e_) for s_, e_, dd in ents if dd is d][0]
        ks, kc = light_keys_range(lines, s, e)
        if new_lines is None:
            was_last = (b + 1 == kc)
            del lines[a:b + 1]
            if was_last and lines[a - 1].endswith(","):
                lines[a - 1] = lines[a - 1][:-1]
            continue
        # existing replacement keeps its own trailing comma status
        replacing = b >= a
        if replacing:
            trailing = lines[b].endswith(",")
            new_lines = list(new_lines)
            if trailing:
                new_lines[-1] += ","
            lines[a:b + 1] = new_lines
        else:
            # insertion before line `a` (a may be the closing brace of Light)
            new_lines = list(new_lines)
            if a == kc:  # appended as the new last key
                lines[a - 1] += ","
            else:
                new_lines[-1] += ","
            lines[a:a] = new_lines
    for r in report:
        print("[%s] %s" % (label, r))
    n_apply = sum(1 for r in report if r.startswith("apply"))
    print("[%s] %d changed, %d already applied" % (label, n_apply, len(report) - n_apply))
    if n_apply and not dry:
        out = nl.join(lines).encode("utf-8")
        json.loads(out)  # must still parse
        with open(scene, "wb") as f:
            f.write(out)
    return n_apply


def scene_arg():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    return a[0] if a else DEFAULT_SCENE
