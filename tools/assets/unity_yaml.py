# A small reader for Unity's serialized YAML (.prefab, .unity, .mat, .meta): just enough of YAML for what Unity writes
# (block mappings and sequences, inline {a: 1, b: {c: 2}} flow mappings, plain scalars), no dependency on PyYAML.
#   docs(text)  -> [(classID, fileID, typeName, body dict, stripped)] for each "--- !u!<class> &<id>" document
#   load(text)  -> the dict of a single-document file (.meta, or the body of a .mat)
import re

_DOC = re.compile(r'^--- !u!(\d+) &(-?\d+)( stripped)?\s*$', re.M)
_GUID = re.compile(r'^[0-9a-f]{32}$')
_ESC = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '"': '"', '\\': '\\', '/': '/'}


def _unescape(m):
    e = m.group(1)
    if e[0] in 'xu' and len(e) > 1:
        return chr(int(e[1:], 16))
    return _ESC.get(e, e)


def _scalar(s):
    s = s.strip()
    if s == '' or s == '~' or s == 'null':
        return None
    if len(s) >= 2 and s[0] == s[-1] and s[0] == "'":
        return s[1:-1].replace("''", "'")
    if len(s) >= 2 and s[0] == s[-1] == '"':
        # double-quoted: Unity escapes non-ASCII ('Wall_1st_\xBDx' is Wall_1st_½x)
        return re.sub(r'\\(x[0-9A-Fa-f]{2}|u[0-9A-Fa-f]{4}|.)', _unescape, s[1:-1])
    if _GUID.match(s):  # a guid, even one that reads as a number ('0000000000000000e000000000000000' is 0.0)
        return s
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        return s


def _flow(s, i=0):
    """Parse an inline value starting at s[i]; returns (value, next index)."""
    while i < len(s) and s[i] == ' ':
        i += 1
    if i < len(s) and s[i] == '{':
        out, i = {}, i + 1
        while True:
            while i < len(s) and s[i] in ' ,':
                i += 1
            if i >= len(s) or s[i] == '}':
                return out, i + 1
            j = s.index(':', i)
            key = s[i:j].strip()
            val, i = _flow(s, j + 1)
            out[key] = val
    if i < len(s) and s[i] == '[':
        out, i = [], i + 1
        while True:
            while i < len(s) and s[i] in ' ,':
                i += 1
            if i >= len(s) or s[i] == ']':
                return out, i + 1
            val, i = _flow(s, i)
            out.append(val)
    j = i
    depth = 0
    while j < len(s):
        c = s[j]
        if c in '{[':
            depth += 1
        elif c in '}]':
            if depth == 0:
                break
            depth -= 1
        elif c == ',' and depth == 0:
            break
        j += 1
    return _scalar(s[i:j]), j


def _value(text):
    t = text.strip()
    if t.startswith('{') or t.startswith('['):
        return _flow(t)[0]
    return _scalar(t)


def _parse_block(lines, i, indent):
    """Parse lines[i:] at the given indent into a dict or list. Returns (value, next line index)."""
    # A sequence may sit at the same indent as its parent key ("key:\n- a\n- b"): Unity writes it that way.
    if i < len(lines) and lines[i][1].startswith('- ') and lines[i][0] == indent:
        out = []
        while i < len(lines) and lines[i][0] == indent and (lines[i][1].startswith('- ') or lines[i][1] == '-'):
            item = lines[i][1][2:] if lines[i][1] != '-' else ''
            m = re.match(r'^([^{\[\'"][^:]*?):(?:\s(.*)|$)', item)
            if m:  # "- key: value" starts a mapping item; its other keys follow indented by 2
                sub = [(indent + 2, item)]
                k = i + 1
                while k < len(lines) and lines[k][0] > indent:
                    sub.append(lines[k])
                    k += 1
                val, _ = _parse_block(sub, 0, indent + 2)
                out.append(val)
                i = k
            else:
                out.append(_value(item))
                i += 1
        return out, i
    out = {}
    while i < len(lines) and lines[i][0] >= indent:
        ind, text = lines[i]
        if ind > indent:  # continuation of a wrapped scalar: append to the last key
            i += 1
            continue
        m = re.match(r'^([^:]+?):(?:\s+(.*)|\s*$)', text)
        if not m:
            i += 1
            continue
        key, rest = m.group(1).strip(), m.group(2)
        i += 1
        if rest is not None and rest.strip() != '':
            v = rest
            # wrapped flow mapping / quoted scalar over several lines
            while (v.count('{') > v.count('}') or v.count('[') > v.count(']')) and i < len(lines):
                v += ' ' + lines[i][1]
                i += 1
            out[key] = _value(v)
        elif i < len(lines) and (lines[i][0] > indent or (lines[i][0] == indent and lines[i][1].startswith('- '))):
            out[key], i = _parse_block(lines, i, lines[i][0])
        else:
            out[key] = None
    return out, i


def _lines(text):
    out = []
    for raw in text.splitlines():
        if not raw.strip() or raw.startswith('%') or raw.startswith('---'):
            continue
        s = raw.lstrip(' ')
        out.append((len(raw) - len(s), s.rstrip()))
    return out


def load(text):
    lines = _lines(text)
    return _parse_block(lines, 0, lines[0][0])[0] if lines else {}


def docs(text):
    out = []
    marks = list(_DOC.finditer(text))
    for n, m in enumerate(marks):
        body = text[m.end():marks[n + 1].start() if n + 1 < len(marks) else len(text)]
        d = load(body)
        (type_name, val), = d.items() if len(d) == 1 else (('?', d),)
        out.append((int(m.group(1)), int(m.group(2)), type_name, val or {}, bool(m.group(3))))
    return out


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()
