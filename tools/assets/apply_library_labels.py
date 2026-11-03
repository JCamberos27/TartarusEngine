"""Label every asset by its project/asset_library.json category, so the Asset Browser's Favorites work.

The browser's Favorites are Unity-style saved searches: "Favorites/Architecture/Walls" is the search l:Walls.
An asset gets its category's label and its parent category's (a wall is Walls and Architecture), both made the
way the editor makes them (AssetLibraryIndex::LabelOf): the category name with only letters and digits. Labels
are added to the .meta's "labels" list; labels already there are kept. The editor labels new imports itself.

usage: apply_library_labels.py <repo> [--dry]
"""
import fnmatch
import json
import os
import sys

KINDS = ('.prefab', '.decal', '.decalpalette', '.hdr')


def label_of(categories, cid):
    for c in categories:
        if c.get('id') == cid and cid != 'other':
            return ''.join(ch for ch in c.get('name', '') if ch.isalnum())
    return ''


def labels_for(categories, cid):
    out = {label_of(categories, cid)}
    for c in categories:
        if c.get('id') == cid and c.get('parent'):
            out.add(label_of(categories, c['parent']))
    return {l for l in out if l}


def category_of(rules, rel):
    for r in rules:
        # The editor's GlobMatch: case-insensitive, and '*' also crosses '/'.
        if fnmatch.fnmatchcase(rel.lower(), r.get('glob', '').replace('**', '*').lower()):
            return r.get('category', 'other')
    return 'other'


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    repo = os.path.abspath(sys.argv[1])
    dry = '--dry' in sys.argv
    project = os.path.join(repo, 'project')
    lib = json.load(open(os.path.join(project, 'asset_library.json'), encoding='utf-8'))
    cats, rules = lib.get('categories', []), lib.get('rules', [])
    counts, changed, missing = {}, 0, 0
    for root, dirs, files in os.walk(os.path.join(project, 'assets')):
        for f in files:
            if not f.lower().endswith(KINDS):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, project).replace(os.sep, '/')
            labels = labels_for(cats, category_of(rules, rel))
            if not labels:
                continue
            meta = path + '.meta'
            if not os.path.exists(meta):
                missing += 1
                continue
            text = open(meta, encoding='utf-8', newline='').read()
            j = json.loads(text)
            have = set(j.get('labels', []))
            if labels <= have:
                continue
            j['labels'] = sorted(have | labels)
            for l in labels:
                counts[l] = counts.get(l, 0) + 1
            changed += 1
            if not dry:
                indent = 2 if '\n  "' in text else (4 if '\n    "' in text else None)
                out = json.dumps(j, indent=indent, ensure_ascii=False) + ('\n' if text.endswith('\n') else '')
                with open(meta, 'w', encoding='utf-8', newline='') as fh:
                    fh.write(out)
    print(f'{changed} .meta file(s) {"would get" if dry else "got"} labels; {missing} asset(s) have no .meta.')
    for l in sorted(counts):
        print(f'  {l}: {counts[l]}')


if __name__ == '__main__':
    main()
