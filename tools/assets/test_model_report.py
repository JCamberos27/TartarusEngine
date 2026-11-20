"""model_report parses the engine's --model-report lines and splits long path lists into several runs."""
import os
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_report  # noqa: E402

OUT = '''[ModelReport] C:/p/Chair.fbx size 0.500 1.000 0.500 min -0.250 0.000 -0.250 max 0.250 1.000 0.250 meshes 2 materials Wood|Metal
[ModelReport]   tris: 1200
[ModelReport] C:/p/Bare.obj size 1.000 1.000 1.000 min 0.000 0.000 0.000 max 1.000 1.000 1.000 meshes 1 materials
[ModelReport] FAILED C:/p/Broken.fbx
[ModelReport] 3 models, 1 failed
'''


class ModelReportTests(unittest.TestCase):
    def test_parse(self):
        rows, failed = model_report.parse(OUT)
        self.assertEqual(failed, 1)
        chair = rows[model_report.key('C:/p/Chair.fbx')]
        self.assertEqual(chair['min'], [-0.25, 0.0, -0.25])
        self.assertEqual(chair['max'], [0.25, 1.0, 0.25])
        self.assertEqual((chair['meshes'], chair['materials'], chair['tris']), (2, ['Wood', 'Metal'], 1200))
        bare = rows[model_report.key('C:/p/Bare.obj')]
        self.assertEqual((bare['materials'], bare['tris']), ([], None))
        self.assertEqual(len(rows), 2)

    def test_no_summary_is_a_failure(self):
        self.assertIsNone(model_report.parse(OUT.rsplit('[ModelReport] 3', 1)[0])[1])

    def test_run_batches_long_lists(self):
        paths = ['C:\\' + 'x' * 300 + f'{i}.fbx' for i in range(200)]
        calls = []

        def run(cmd, **kw):
            calls.append(cmd[2:])
            return subprocess.CompletedProcess(cmd, 0, stdout=f'[ModelReport] {len(cmd) - 2} models, 0 failed\n', stderr='')

        with mock.patch.object(model_report.subprocess, 'run', side_effect=run):
            rows, failed, _ = model_report.run('engine.exe', paths)
        self.assertGreater(len(calls), 1)
        self.assertEqual(sum(len(c) for c in calls), 200)
        self.assertTrue(all(sum(len(p) + 3 for p in c) <= 24000 for c in calls))
        self.assertEqual(failed, 0)


if __name__ == '__main__':
    unittest.main()
