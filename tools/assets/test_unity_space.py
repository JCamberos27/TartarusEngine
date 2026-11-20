"""read_nodes asks the engine again only for the FBX files a --model-nodes run left out, and keeps each command short."""
import os
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_space  # noqa: E402

LOCAL = ' '.join(['1', '0', '0', '0', '0', '1', '0', '0', '0', '0', '1', '0', '0', '0', '0', '1'])


def answer(paths):
    lines = []
    for p in paths:
        lines.append(f'[ModelNodes] {p} unitscale 1')
        lines.append(f'[ModelNodes]   node Root parent - local {LOCAL}')
    return '\n'.join(lines) + '\n'


class ReadNodesTests(unittest.TestCase):
    def test_retries_only_the_dropped_files(self):
        files = [os.path.join('C:\\pack', f'm{i}.fbx') for i in range(5)]
        calls = []

        def run(cmd, **kw):
            batch = cmd[2:]
            calls.append(batch)
            shown = batch if len(calls) > 1 else batch[:-2]   # the first run drops its last two files
            return subprocess.CompletedProcess(cmd, 0, stdout=answer(shown), stderr='')

        with mock.patch.object(unity_space.subprocess, 'run', side_effect=run):
            out = unity_space.read_nodes('engine.exe', files)
        self.assertEqual(len(out), 5)
        self.assertEqual(calls[1], sorted(files)[-2:])
        self.assertEqual(out[os.path.normcase(files[0])]['order'], ['Root'])

    def test_unanswered_files_stop_after_three_tries(self):
        calls = []

        def run(cmd, **kw):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout='', stderr='')

        with mock.patch.object(unity_space.subprocess, 'run', side_effect=run), mock.patch('sys.stderr'):
            out = unity_space.read_nodes('engine.exe', ['C:\\pack\\broken.fbx'])
        self.assertEqual(out, {})
        self.assertEqual(len(calls), 3)

    def test_batches_stay_under_the_command_line_limit(self):
        files = ['C:\\' + 'x' * 200 + f'{i}.fbx' for i in range(400)]
        batches = list(unity_space._batches(files))
        self.assertEqual(sum(len(b) for b in batches), 400)
        self.assertTrue(all(sum(len(f) + 3 for f in b) <= 24000 for b in batches))


if __name__ == '__main__':
    unittest.main()
