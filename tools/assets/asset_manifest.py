# Third-party assets that live on the team's shared Google Drive instead of in git (docs/ASSETS.md).
#
#   python tools/assets/asset_manifest.py build <repo>              writes <repo>/project/external_assets.csv
#   python tools/assets/asset_manifest.py export <repo> <Used dir>  copies every listed file into <Used dir>
#   python tools/assets/asset_manifest.py verify <dir> [--hash]     checks <dir> (a repo or a Used folder)
#
# "External" = every file under project/assets that .gitignore excludes, whether git still tracks it
# or not, minus local leftovers (EXCLUDE). The CSV holds project-relative paths, sizes and SHA-256s;
# the editor reads it so a missing file keeps its tracked .meta (AssetDatabase::ScanProject), and
# tools/assets/fetch-assets.ps1 uses it to report what a clone is still missing.
# Paths are written in the case git tracks them, even when a folder's case differs on disk.
import csv, hashlib, os, shutil, subprocess, sys

CSV_REL = os.path.join('project', 'external_assets.csv')
EXCLUDE_PREFIXES = (
    'project/assets/Characters/Quantum/Textures/Clothing/Hats/',  # hats were removed from the project (#501)
    'project/assets/Characters/ybot/',  # a local Mixamo experiment no committed scene uses (Raw\Animations)
)
EXCLUDE_SUFFIXES = ('.bak', '.recovery.json', 'desktop.ini', 'Thumbs.db')


def git(repo, *args):
    out = subprocess.run(['git', '-C', repo, '-c', 'core.quotepath=off', *args],
                         capture_output=True, text=True, encoding='utf-8', check=True).stdout
    return [line for line in out.split('\0') if line]


def canonical_case(repo, paths):
    # Map every directory git tracks to its tracked spelling, then respell each path's folders.
    canon = {}
    for p in git(repo, 'ls-files', '-z', '--', 'project'):
        parts = p.split('/')[:-1]
        for i in range(1, len(parts) + 1):
            canon.setdefault('/'.join(parts[:i]).lower(), '/'.join(parts[:i]))
    fixed = []
    for p in paths:
        parts = p.split('/')
        out = parts[:]
        for i in range(len(parts) - 1, 0, -1):
            key = '/'.join(parts[:i]).lower()
            if key in canon:
                out[:i] = canon[key].split('/')
                break
        fixed.append('/'.join(out))
    return fixed


def external_files(repo):
    ignored = git(repo, 'ls-files', '-z', '--others', '--ignored', '--exclude-standard', '--', 'project/assets')
    tracked = git(repo, 'ls-files', '-z', '--cached', '--ignored', '--exclude-standard', '--', 'project/assets')
    keep = [p for p in set(ignored) | set(tracked)
            if not any(p.lower().startswith(x.lower()) for x in EXCLUDE_PREFIXES)
            and not p.endswith(EXCLUDE_SUFFIXES)
            and os.path.isfile(os.path.join(repo, p))]
    pairs = sorted(zip(canonical_case(repo, keep), keep), key=lambda t: t[0].lower())
    return pairs  # (canonical repo-relative path, on-disk repo-relative path)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def build(repo):
    pairs = external_files(repo)
    out = os.path.join(repo, CSV_REL)
    with open(out, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['path', 'size', 'sha256'])
        for canon, disk in pairs:
            full = os.path.join(repo, disk)
            w.writerow([canon[len('project/'):], os.path.getsize(full), sha256(full)])
    total = sum(os.path.getsize(os.path.join(repo, d)) for _, d in pairs)
    print(f'{out}: {len(pairs)} files, {total / (1 << 30):.2f} GB')


def read_manifest(repo_or_used):
    with open(os.path.join(repo_or_used, CSV_REL), newline='', encoding='utf-8') as f:
        return [(r['path'], int(r['size']), r['sha256']) for r in csv.DictReader(f)]


def export(repo, used):
    rows = read_manifest(repo)
    on_disk = {c[len('project/'):].lower(): d for c, d in external_files(repo)}
    copied = 0
    for rel, size, _ in rows:
        src = os.path.join(repo, on_disk.get(rel.lower(), 'project/' + rel))
        dst = os.path.join(used, 'project', rel)
        if os.path.isfile(dst) and os.path.getsize(dst) == size:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        copied += 1
    os.makedirs(os.path.join(used, 'project'), exist_ok=True)
    shutil.copy2(os.path.join(repo, CSV_REL), os.path.join(used, CSV_REL))
    print(f'{used}: {copied} copied, {len(rows) - copied} already up to date')


def verify(root, check_hash):
    rows = read_manifest(root)
    missing, wrong = [], []
    for rel, size, digest in rows:
        p = os.path.join(root, 'project', rel)
        if not os.path.isfile(p):
            missing.append(rel)
        elif os.path.getsize(p) != size or (check_hash and sha256(p) != digest):
            wrong.append(rel)
    for rel in missing[:20]: print('missing  ', rel)
    for rel in wrong[:20]: print('different', rel)
    print(f'{root}: {len(rows)} listed, {len(missing)} missing, {len(wrong)} different')
    return 1 if missing or wrong else 0


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'build':
        build(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'export':
        export(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'verify':
        sys.exit(verify(sys.argv[2], '--hash' in sys.argv))
    else:
        print(__doc__ or open(__file__).read().split('\nimport')[0])
        sys.exit(2)
