# Third-party assets that live on the team's shared Google Drive instead of in git.
#
# Each external file is tracked by a hash kept in its own <file>.meta ("payload": {"sha256", "size"}), and the
# Drive is a content-addressed store: <Store>/<sha[0:2]>/<sha256><lowercase ext>. A file is never renamed or
# moved on the Drive when it moves in git, so nothing is re-uploaded.
#
#   python tools/assets/asset_manifest.py build <repo>                  writes "payload" into every external file's .meta
#   python tools/assets/asset_manifest.py import-csv <repo>             seeds .meta payloads from the old project/external_assets.csv (no hashing)
#   python tools/assets/asset_manifest.py export <repo> <Store> [--only <prefix>]   copies each payload into the store
#   python tools/assets/asset_manifest.py verify <dir> [--hash]         a repo: payload files present (and matching with --hash)
#   python tools/assets/asset_manifest.py verify <Store> --repo <repo>  a store: every repo payload hash is in it; reports unreferenced files
#   python tools/assets/asset_manifest.py migrate-store <Used> <Store> [--dry] [--move]   files of an old Used\project mirror -> store layout
#
# "External" = every file under project/assets or project/scenes that .gitignore excludes, whether git still tracks it
# or not, minus local leftovers (EXCLUDE). A file with no .meta is skipped and reported (this tool never creates one).
# build is merge-preserving: only the "payload" key of the .meta changes, with its indent and line endings kept,
# and the key is removed from a .meta whose file is no longer external. Hashes are cached by (path, size, mtime)
# in project/Library/payload_hash_cache.json. tools/assets/fetch-assets.ps1 restores missing payloads from the store.
# migrate-store writes <Store>/store_migration.log ("sha256<TAB>ext<TAB>original relpath") so a run can be rolled back.
# Paths are written in the case git tracks them, even when a folder's case differs on disk.
import csv, hashlib, json, os, shutil, subprocess, sys

CSV_REL = os.path.join('project', 'external_assets.csv')  # retired; only import-csv reads it
CACHE_REL = os.path.join('project', 'Library', 'payload_hash_cache.json')
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
    ignored = git(repo, 'ls-files', '-z', '--others', '--ignored', '--exclude-standard', '--', 'project/assets', 'project/scenes')
    tracked = git(repo, 'ls-files', '-z', '--cached', '--ignored', '--exclude-standard', '--', 'project/assets', 'project/scenes')
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


def load_cache(repo):
    try:
        with open(os.path.join(repo, CACHE_REL), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_cache(repo, cache):
    p = os.path.join(repo, CACHE_REL)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8') as f:
        json.dump(cache, f)


def cached_sha(cache, key, full):
    st = os.stat(full)
    hit = cache.get(key)
    if hit and hit[0] == st.st_size and hit[1] == st.st_mtime_ns:
        return hit[2]
    digest = sha256(full)
    cache[key] = [st.st_size, st.st_mtime_ns, digest]
    return digest


def store_path(store, digest, name):
    return os.path.join(store, digest[:2], digest + os.path.splitext(name)[1].lower())


def read_meta(path):
    """(parsed JSON, indent, newline, text) of a .meta file, or None when it isn't a JSON object."""
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8')
    newline = '\r\n' if '\r\n' in text else '\n'
    indent = 2
    for line in text.split('\n')[1:]:
        stripped = line.lstrip(' ')
        if stripped and len(stripped) != len(line):
            indent = len(line) - len(stripped)
            break
    try:
        data = json.loads(text)
    except ValueError:
        return None
    return (data, indent, newline, text) if isinstance(data, dict) else None


def write_meta(path, data, indent, newline, old_text):
    text = json.dumps(data, indent=indent, ensure_ascii=False)
    if newline != '\n':
        text = text.replace('\n', newline)
    if old_text.endswith('\n'):
        text += newline
    if text == old_text:
        return False
    with open(path, 'wb') as f:
        f.write(text.encode('utf-8'))
    return True


def set_payload(meta_path, payload):
    """Set (a dict) or remove (None) the "payload" key of a .meta. Returns 'changed', 'same' or 'bad'."""
    r = read_meta(meta_path)
    if r is None:
        return 'bad'
    data, indent, newline, text = r
    if payload is None:
        if 'payload' not in data:
            return 'same'
        del data['payload']
    else:
        if data.get('payload') == payload:
            return 'same'
        data['payload'] = payload  # replaces in place, or appends as the last key
    return 'changed' if write_meta(meta_path, data, indent, newline, text) else 'same'


def payload_metas(repo):
    """Yield (meta path, payload dict) for every .meta under project/ (not Library) that has a payload."""
    base = os.path.join(repo, 'project')
    for parent, dirs, files in os.walk(base):
        if parent == base:
            dirs[:] = [d for d in dirs if d != 'Library']
        for name in files:
            if not name.endswith('.meta'):
                continue
            p = os.path.join(parent, name)
            try:
                with open(p, encoding='utf-8') as f:
                    text = f.read()
                if '"payload"' not in text:
                    continue
                pl = json.loads(text).get('payload')
            except (OSError, ValueError, AttributeError):
                continue
            if isinstance(pl, dict) and 'sha256' in pl:
                yield p, pl


def build(repo):
    pairs = external_files(repo)
    cache, fresh = load_cache(repo), {}
    changed = same = 0
    nometa, bad = [], []
    external_metas = set()
    for canon, disk in pairs:
        full = os.path.join(repo, disk)
        meta = full + '.meta'
        if not os.path.isfile(meta):
            nometa.append(canon)
            continue
        external_metas.add(os.path.normcase(os.path.abspath(meta)))
        digest = cached_sha(cache, disk, full)
        fresh[disk] = cache[disk]
        r = set_payload(meta, {'sha256': digest, 'size': os.path.getsize(full)})
        if r == 'bad': bad.append(canon)
        elif r == 'changed': changed += 1
        else: same += 1
    save_cache(repo, fresh)
    removed = 0
    for meta, _ in list(payload_metas(repo)):
        if os.path.normcase(os.path.abspath(meta)) not in external_metas and set_payload(meta, None) == 'changed':
            removed += 1
    for rel in nometa[:20]: print('no .meta ', rel)
    for rel in bad[:20]: print('bad .meta', rel)
    print(f'{repo}: {len(pairs)} external files, {changed} .meta updated, {same} unchanged, '
          f'{len(nometa)} without a .meta, {len(bad)} unreadable .meta, {removed} stale payload(s) removed')


def read_csv(repo):
    """The retired project/external_assets.csv as (project-relative path, size, sha256) rows."""
    with open(os.path.join(repo, CSV_REL), newline='', encoding='utf-8') as f:
        return [(r['path'], int(r['size']), r['sha256']) for r in csv.DictReader(f)]


def import_csv(repo):
    done = same = 0
    nometa = []
    for rel, size, digest in read_csv(repo):
        meta = os.path.join(repo, 'project', rel + '.meta')
        if not os.path.isfile(meta):
            nometa.append(rel)
            continue
        if set_payload(meta, {'sha256': digest, 'size': size}) == 'changed': done += 1
        else: same += 1
    for rel in nometa[:20]: print('no .meta ', rel)
    print(f'{repo}: {done} .meta payloads seeded, {same} unchanged, {len(nometa)} without a .meta')


def export(repo, store, only=None):
    on_disk = {c.lower(): d for c, d in external_files(repo)}
    copied = present = skipped = 0
    for meta, pl in payload_metas(repo):
        target = meta[:-len('.meta')]
        rel = os.path.relpath(target, repo).replace(os.sep, '/')
        if only and not rel.lower().startswith(only.lower()):
            continue
        src = os.path.join(repo, on_disk.get(rel.lower(), rel))
        if not os.path.isfile(src):
            skipped += 1
            continue
        dst = store_path(store, pl['sha256'], src)
        if os.path.isfile(dst):
            if os.path.getsize(dst) == pl['size']:
                present += 1
                continue
            os.remove(dst)  # a truncated earlier copy
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        if os.path.getsize(dst) != pl['size']:
            raise SystemExit(f'size mismatch after copying {rel} -> {dst}')
        copied += 1
    print(f'{store}: {copied} copied, {present} already in the store, {skipped} not on disk here')


def verify(root, check_hash, repo=None):
    if repo is not None or not os.path.exists(os.path.join(root, '.git')):
        return verify_store(root, repo)
    total = 0
    missing, wrong = [], []
    for meta, pl in payload_metas(root):
        total += 1
        p = meta[:-len('.meta')]
        rel = os.path.relpath(p, root).replace(os.sep, '/')
        if not os.path.isfile(p):
            missing.append(rel)
        elif os.path.getsize(p) != pl['size'] or (check_hash and sha256(p) != pl['sha256']):
            wrong.append(rel)
    for rel in missing[:20]: print('missing  ', rel)
    for rel in wrong[:20]: print('different', rel)
    print(f'{root}: {total} payloads, {len(missing)} missing, {len(wrong)} different')
    return 1 if missing or wrong else 0


def verify_store(store, repo):
    if repo is None:
        print('verifying a store needs --repo <repo>')
        return 2
    wanted = {}
    for meta, pl in payload_metas(repo):
        wanted.setdefault(pl['sha256'], meta[:-len('.meta')])
    have = {}
    for parent, _, files in os.walk(store):
        for name in files:
            if name != 'store_migration.log' and not name.endswith(EXCLUDE_SUFFIXES):
                have[os.path.splitext(name)[0]] = os.path.join(parent, name)
    missing = [(h, p) for h, p in wanted.items() if h not in have]
    unref = [p for h, p in have.items() if h not in wanted]
    for h, p in missing[:20]: print('missing     ', h, os.path.relpath(p, repo))
    for p in unref[:20]: print('unreferenced', os.path.relpath(p, store))
    print(f'{store}: {len(wanted)} payload hashes, {len(missing)} missing from the store, {len(unref)} unreferenced')
    return 1 if missing else 0


def migrate_store(used, store, dry=False, move=False):
    base = os.path.join(used, 'project')
    placed, dups, seen = [], [], set()
    for parent, _, files in os.walk(base):
        for name in sorted(files):
            if name.endswith(EXCLUDE_SUFFIXES):
                continue
            src = os.path.join(parent, name)
            rel = os.path.relpath(src, used).replace(os.sep, '/')
            digest = sha256(src)
            dst = store_path(store, digest, name)
            if os.path.exists(dst) or dst in seen:
                dups.append((rel, dst))
                continue
            seen.add(dst)
            if not dry:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                (shutil.move if move else shutil.copy2)(src, dst)
                with open(os.path.join(store, 'store_migration.log'), 'a', encoding='utf-8', newline='\n') as log:
                    log.write(f'{digest}\t{os.path.splitext(name)[1].lower()}\t{rel}\n')
            placed.append(rel)
    for rel, _ in dups[:20]: print('duplicate (left in place)', rel)
    verb = 'would be ' if dry else ''
    print(f'{used}: {len(placed)} {verb}{"moved" if move else "copied"} to {store}, {len(dups)} duplicate(s) left where they were')


def opt(args, flag):
    i = args.index(flag) + 1 if flag in args else 0
    return args[i] if 0 < i < len(args) else None


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'build':
        build(a[1])
    elif len(a) >= 2 and a[0] == 'import-csv':
        import_csv(a[1])
    elif len(a) >= 3 and a[0] == 'export':
        export(a[1], a[2], opt(a, '--only'))
    elif len(a) >= 2 and a[0] == 'verify':
        sys.exit(verify(a[1], '--hash' in a, opt(a, '--repo')))
    elif len(a) >= 3 and a[0] == 'migrate-store':
        migrate_store(a[1], a[2], '--dry' in a, '--move' in a)
    else:
        print(open(__file__, encoding='utf-8').read().split('\nimport')[0])
        sys.exit(2)
