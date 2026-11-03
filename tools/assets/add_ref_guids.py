# Adds GUIDs beside path references in existing project files so the engine can follow moved files.
# A path's GUID is the "guid" of "<path>.meta" next to the file under project/. A ref may carry a "#suffix" (stripped
# to find the file). A ref with no .meta is skipped and counted.
#   .decal  : "textureGuids": {albedo|normal|surface|aux: guid}, placed right after "textures"
#   .outfit : top-level "wardrobeGuid", and "itemGuid" in each "items" entry
#   assets/Audio/audio_manifest.json : "guid" for each entry's "file" (relative to project/assets/Audio)
#
#   python tools/assets/add_ref_guids.py [<repo>] [--dry]
# Formatting is kept (indent, key order, line endings, trailing newline); only files that change are written.
import json
import os
import re
import sys
from collections import OrderedDict

_guids = {}


def guid_of(project, path):
    path = path.split("#", 1)[0]
    if path not in _guids:
        guid = None
        try:
            with open(os.path.join(project, path + ".meta"), encoding="utf-8-sig") as f:
                guid = json.load(f).get("guid") or None
        except (OSError, ValueError):
            pass
        _guids[path] = guid
    return _guids[path]


def detect_indent(text):
    m = re.search(r"\n([ \t]+)\S", text)
    if not m:
        return None
    ws = m.group(1)
    return ws if ws[0] == "\t" else len(ws)


def load(path):
    with open(path, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    eol = "\r\n" if "\r\n" in text else "\n"
    return json.loads(text, object_pairs_hook=OrderedDict), text, eol, bom


def save(path, data, text, eol, bom):
    out = json.dumps(data, indent=detect_indent(text), ensure_ascii=False)
    if text.endswith("\n"):
        out += "\n"
    out = out.replace("\n", eol)
    with open(path, "wb") as f:
        f.write((b"\xef\xbb\xbf" if bom else b"") + out.encode("utf-8"))


class Stats:
    def __init__(self):
        self.files = 0
        self.given = 0
        self.skipped = 0
        self.skipped_paths = []

    def ref(self, project, path):
        g = guid_of(project, path)
        if g:
            self.given += 1
        else:
            self.skipped += 1
            self.skipped_paths.append(path)
        return g


def fix_decal(project, d, st):
    tex = d.get("textures")
    if not isinstance(tex, dict):
        return d
    guids = OrderedDict()
    for k, p in tex.items():
        if isinstance(p, str) and p:
            g = st.ref(project, p)
            if g:
                guids[k] = g
    out = OrderedDict()
    for k, v in d.items():
        if k == "textureGuids":
            continue
        out[k] = v
        if k == "textures" and guids:
            out["textureGuids"] = guids
    return out


def fix_outfit(project, d, st):
    out = OrderedDict()
    for k, v in d.items():
        if k == "wardrobeGuid":
            continue
        out[k] = v
        if k == "wardrobe" and isinstance(v, str) and v:
            g = st.ref(project, v)
            if g:
                out["wardrobeGuid"] = g
    items = out.get("items")
    if isinstance(items, dict):
        for name, e in items.items():
            if isinstance(e, dict) and isinstance(e.get("item"), str) and e["item"]:
                g = st.ref(project, e["item"])
                items[name] = with_guid(e, "item", "itemGuid", g)
    return out


def with_guid(e, after, key, g):
    out = OrderedDict()
    for k, v in e.items():
        if k == key:
            continue
        out[k] = v
        if k == after and g:
            out[key] = g
    return out


def audio_path(f):
    return f if f.startswith(("assets/", "assets\\")) else "assets/Audio/" + f


def fix_audio(project, d, st):
    def entry(e):
        f = e.get("file") if isinstance(e, dict) else None
        if not isinstance(f, str) or not f:
            return e
        return with_guid(e, "file", "guid", st.ref(project, audio_path(f)))

    for key in ("files", "entries"):
        if isinstance(d.get(key), list):
            d[key] = [entry(e) for e in d[key]]
        elif isinstance(d.get(key), dict):
            for fname, v in list(d[key].items()):
                if isinstance(v, dict):
                    g = st.ref(project, audio_path(fname))
                    v2 = OrderedDict(v)
                    v2.pop("guid", None)
                    if g:
                        v2["guid"] = g
                    d[key][fname] = v2
    return d


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry" in sys.argv
    repo = args[0] if args else os.getcwd()
    project = os.path.join(repo, "project")
    jobs = []
    for root, _, names in os.walk(project):
        for n in names:
            p = os.path.join(root, n)
            if n.endswith(".decal"):
                jobs.append(("decal", p, fix_decal))
            elif n.endswith(".outfit"):
                jobs.append(("outfit", p, fix_outfit))
    jobs.append(("audio", os.path.join(project, "assets", "Audio", "audio_manifest.json"), fix_audio))
    stats = {}
    for kind, path, fn in jobs:
        st = stats.setdefault(kind, Stats())
        if not os.path.isfile(path):
            continue
        data, text, eol, bom = load(path)
        before = json.dumps(data)
        new = fn(project, data, st)
        if json.dumps(new) != before:
            st.files += 1
            if not dry:
                save(path, new, text, eol, bom)
    for kind, st in stats.items():
        print(f"{kind}: files changed {st.files}, refs given GUIDs {st.given}, skipped (no .meta) {st.skipped}")
        for p in sorted(set(st.skipped_paths))[:5]:
            print("   no .meta:", p)
    print("(dry run, nothing written)" if dry else "done")


if __name__ == "__main__":
    main()
