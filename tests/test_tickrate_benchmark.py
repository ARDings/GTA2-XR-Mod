"""Input-validation and CLI contract tests; every timing fixture is synthetic."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('tick_benchmark', Path(__file__).resolve().parents[1] / 'tools/benchmark_gta2_tickrate.py')
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class TickBenchmarkTests(unittest.TestCase):
    def setUp(self):
        root = Path(os.environ.get('GTA2CEILING_WORK_ROOT') or 'D:/GTA2Ceiling-work') / 'temp' / 'tick-benchmark-tests'
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def csv(self, text, name='ticks.csv'):
        path = self.root / name
        path.write_text(text, encoding='utf-8')
        return path

    def invoke(self, path, *options):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = benchmark.main([str(path), '--source', 'synthetic unit fixture',
                                   '--provenance', 'unit test; no device', *options])
        return code, json.loads(output.getvalue())

    def test_timestamps_and_deltas_match(self):
        timestamp = self.csv('timestamp_ms\n1000\n1033\n1066\n1099\n')
        delta = self.csv('delta_ms\n33\n33\n33\n', 'deltas.csv')
        a, _, rows = benchmark.read_tick_csv(timestamp)
        b, _, _ = benchmark.read_tick_csv(delta)
        self.assertEqual(a, b)
        self.assertEqual(rows, 4)
        result = benchmark.evaluate_tick_log(a)
        self.assertAlmostEqual(result['measured_tickrate_hz'], 1000 / 33)
        self.assertEqual(result['duration_seconds'], 0.099)
        self.assertTrue(result['check_passed'])

    def test_percentiles_and_rate_use_intervals(self):
        report = benchmark.evaluate_tick_log([10, 20, 30, 40, 100])
        self.assertEqual(report['average_tick_delta_ms'], 40)
        self.assertEqual(report['measured_tickrate_hz'], 25)
        self.assertEqual(report['p50_tick_delta_ms'], 30)
        self.assertAlmostEqual(report['p95_tick_delta_ms'], 88)
        self.assertAlmostEqual(report['p99_tick_delta_ms'], 97.6)
        self.assertFalse(report['check_passed'])

    def test_invalid_intervals_are_rejected(self):
        for values in ([], [0], [-1], [float('nan')], [float('inf')], [1e308, 1e308]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                benchmark.evaluate_tick_log(values)

    def test_bad_csv_is_rejected(self):
        for data in ('timestamp_ms\n33\n33\n', 'timestamp_ms\n34\n33\n',
                     'timestamp_ms\n-1\n33\n', 'timestamp_ms\n0\n',
                     'delta_ms\nNaN\n', 'delta_ms\ninf\n', 'delta_ms\n0\n',
                     'delta_ms\n-33\n', 'delta_ms\n\n', 'delta_ms\n33,44\n',
                     'timestamp_ms,delta_ms\n0,33\n33,33\n', 'delta_ms,delta_ms\n33,33\n'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                benchmark.read_tick_csv(self.csv(data))

    def test_explicit_column_and_metadata(self):
        path = self.csv('timestamp_ms,delta_ms,scene\n0,33,menu\n33,34,street\n')
        samples, column, rows = benchmark.read_tick_csv(path, 'delta_ms')
        self.assertEqual((samples, column, rows), ([33, 34], 'delta_ms', 2))

    def test_cli_records_provenance_and_checks(self):
        path = self.csv('delta_ms\n33\n33\n')
        code, report = self.invoke(path, '--check')
        self.assertEqual(code, 0)
        self.assertEqual(report['input_sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(report['source'], 'synthetic unit fixture')
        self.assertEqual(report['provenance_verification'], 'operator_supplied_not_independently_verified')
        code, report = self.invoke(path, '--check', '--min-duration-seconds', '300')
        self.assertEqual(code, 1)
        self.assertTrue(report['within_tolerance'])
        self.assertFalse(report['sufficient_duration'])
        code, report = self.invoke(self.csv('delta_ms\n66\n'), '--check')
        self.assertEqual(code, 1)
        self.assertFalse(report['within_tolerance'])

    def test_cli_errors_never_succeed(self):
        for path in (self.root / 'missing.csv', self.csv('delta_ms\nNaN\n')):
            code, report = self.invoke(path)
            self.assertEqual(code, 2)
            self.assertFalse(report['valid'])
            self.assertFalse(report['check_passed'])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            benchmark.main([])
        self.assertEqual(error.exception.code, 2)

    def test_invalid_configuration_is_rejected(self):
        for kwargs in ({'target_hz': 0}, {'target_hz': float('nan')},
                       {'tolerance_percent': -1}, {'min_duration_seconds': float('inf')}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                benchmark.evaluate_tick_log([33], **kwargs)


if __name__ == '__main__':
    unittest.main()
