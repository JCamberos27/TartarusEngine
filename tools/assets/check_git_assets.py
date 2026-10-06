"""Reject tracked payloads that the project's asset policy keeps outside Git.

Run before committing or pushing: python tools/assets/check_git_assets.py
The index is checked, so force-added files cannot bypass .gitignore.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    result = subprocess.run(
        ['git', '-C', str(ROOT), 'ls-files', '-z', '--cached', '--ignored',
         '--exclude-standard', '--', 'project/assets'],
        capture_output=True, check=True,
    )
    forbidden = [path.decode('utf-8') for path in result.stdout.split(b'\0') if path]
    if forbidden:
        print('Asset policy FAILED: ignored third-party payloads are tracked:')
        for path in forbidden:
            print('  ' + path)
        print('Remove these from the index; keep the local files and their .meta sidecars.')
        return 1
    print('Asset policy OK: no ignored project asset payloads are tracked.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
