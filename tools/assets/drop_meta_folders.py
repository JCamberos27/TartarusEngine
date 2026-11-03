"""Retire the Asset Browser's old browser-only folders: strip the "folder" field from every .meta.

The browser now lists the disk, like Unity's Project window (an asset's folder is the folder its file is in;
moving it in the browser moves the file). A .meta "folder" from before is ignored by the editor, so this only
tidies the data - but first it reports every asset whose pinned folder differs from where its file really is,
since those will appear in a different place than before.

usage: drop_meta_folders.py <repo> [--dry] [--report <file.md>]
  --dry     report only, change nothing
  --report  also write the list of differing assets as a Markdown table
"""
import json
import os
import sys


def meta_files(project):
    for root, dirs, files in os.walk(project):
        if os.path.relpath(root, project).split(os.sep)[0] in ('Library', 'screenshots'):
            dirs[:] = []
            continue
        for f in files:
            if f.endswith('.meta'):
                yield os.path.join(root, f)


def disk_folder(project, meta):
    asset = meta[:-5]
    return os.path.relpath(os.path.dirname(asset), project).replace(os.sep, '/')


def strip_folder(text):
    """The .meta text without its "folder" key, keeping the rest of the file as written."""
    j = json.loads(text)
    j.pop('folder', None)
    indent = 2 if '\n  "' in text else (4 if '\n    "' in text else None)
    out = json.dumps(j, indent=indent, ensure_ascii=False)
    return out + ('\n' if text.endswith('\n') else '')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    repo = os.path.abspath(sys.argv[1])
    dry = '--dry' in sys.argv
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else None
    project = os.path.join(repo, 'project')
    pinned = same = changed = bad = 0
    differing = []
    for meta in meta_files(project):
        try:
            text = open(meta, encoding='utf-8', newline='').read()
            j = json.loads(text)
        except (OSError, ValueError):
            bad += 1
            continue
        if not isinstance(j, dict) or 'folder' not in j:
            continue
        pinned += 1
        disk = disk_folder(project, meta)
        if str(j['folder']).replace('\\', '/').strip('/').lower() == disk.lower():
            same += 1
        else:
            differing.append((os.path.relpath(meta[:-5], project).replace(os.sep, '/'), j['folder'], disk))
        if not dry:
            new = strip_folder(text)
            if new != text:
                with open(meta, 'w', encoding='utf-8', newline='') as f:
                    f.write(new)
                changed += 1
    print(f'{pinned} .meta file(s) pin a folder: {same} match their disk folder, {len(differing)} differ'
          f'{"" if dry else f"; {changed} rewritten"}{f"; {bad} unreadable" if bad else ""}.')
    for asset, was, now in differing[:20]:
        print(f'  {asset}: shown in "{was}", on disk in "{now}"')
    if len(differing) > 20:
        print(f'  ... and {len(differing) - 20} more')
    if report:
        with open(report, 'w', encoding='utf-8') as f:
            f.write('# Assets whose browser folder differs from their disk folder\n\n')
            f.write(f'{len(differing)} of {pinned} pinned assets. After the change each shows in its disk folder.\n\n')
            f.write('| Asset | Shown in (old) | On disk (new) |\n|---|---|---|\n')
            for asset, was, now in differing:
                f.write(f'| `{asset}` | `{was}` | `{now}` |\n')
        print('report:', report)


if __name__ == '__main__':
    main()
