"""Tests for results.py using synthetic CSVs. Run with: make test"""

import os
import sys
import tempfile
import unittest
from io import StringIO
from contextlib import redirect_stdout, redirect_stderr

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import results  # noqa: E402

HEADER = "timestamp,gpu_idle_fraction,cpu_slice_us,iter_latency_us,runnable_tasks"


def write_csv(lines, name="m.csv"):
    d = tempfile.mkdtemp()
    path = os.path.join(d, name)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


class NearestRankTests(unittest.TestCase):
    def test_p50_and_p95_on_known_values(self):
        vals = list(range(1, 21))  # 1..20
        self.assertEqual(results.nearest_rank(vals, 0.50), 10)
        self.assertEqual(results.nearest_rank(vals, 0.95), 19)

    def test_single_value(self):
        self.assertEqual(results.nearest_rank([7], 0.95), 7)


class LoadTests(unittest.TestCase):
    def test_rejects_old_schema(self):
        path = write_csv(["timestamp,gpu_idle_fraction,step_time_us,iter_latency_us,runnable_tasks",
                          "1,0,1,1,1", "2,0,1,1,1", "3,0,1,1,1"])
        with self.assertRaisesRegex(results.InputError, "cpu_slice_us"):
            results.load_csv(path, 0)

    def test_rejects_non_numeric(self):
        path = write_csv([HEADER, "1,0,5,5,1", "2,0,x,5,1", "3,0,5,5,1"])
        with self.assertRaisesRegex(results.InputError, "non-numeric"):
            results.load_csv(path, 0)

    def test_rejects_non_increasing_timestamps(self):
        path = write_csv([HEADER, "2,0,5,5,1", "1,0,5,5,1", "3,0,5,5,1"])
        with self.assertRaisesRegex(results.InputError, "strictly increasing"):
            results.load_csv(path, 0)

    def test_rejects_when_warmup_consumes_all_rows(self):
        path = write_csv([HEADER, "1,0,5,5,1", "2,0,5,5,1"])
        with self.assertRaisesRegex(results.InputError, "warm-up"):
            results.load_csv(path, 2)

    def test_missing_file(self):
        with self.assertRaisesRegex(results.InputError, "not found"):
            results.load_csv("/nonexistent/none.csv", 0)

    def test_warmup_is_dropped(self):
        path = write_csv([HEADER, "1,900,5,5,1", "2,900,5,5,1", "3,100,5,5,1"])
        cols, dropped = results.load_csv(path, 2)
        self.assertEqual(dropped, 2)
        self.assertEqual(cols["gpu_idle_fraction"], [100.0])


class CorrelationTests(unittest.TestCase):
    def test_perfect_positive(self):
        self.assertAlmostEqual(results.pearson([1, 2, 3], [2, 4, 6]), 1.0)

    def test_too_few_samples_is_none(self):
        self.assertIsNone(results.pearson([1], [1]))

    def test_constant_series_is_zero(self):
        self.assertEqual(results.pearson([1, 1, 1], [1, 2, 3]), 0.0)


class EndToEndTests(unittest.TestCase):
    def _run(self, argv):
        out, err = StringIO(), StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = results.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_summary_and_baseline(self):
        main = write_csv([HEADER] + [f"{i},500,{100 + i},200,4" for i in range(1, 12)])
        base = write_csv([HEADER] + [f"{i},400,{200 + i},200,4" for i in range(1, 12)], "b.csv")
        code, out, _ = self._run(["--warmup", "2", "--min-samples", "5", main, base])
        self.assertEqual(code, 0)
        self.assertIn("Warm-up samples excluded: 2", out)
        self.assertIn("Samples analyzed: 9", out)
        self.assertIn("Samples: scheduler=9, baseline=9", out)
        self.assertNotIn("WARNING", out)

    def test_small_sample_warning(self):
        main = write_csv([HEADER, "1,0,5,5,1", "2,0,5,5,1", "3,0,5,5,1"])
        base = write_csv([HEADER, "1,0,5,5,1", "2,0,5,5,1", "3,0,5,5,1"], "b.csv")
        code, out, _ = self._run(["--warmup", "0", "--min-samples", "30", main, base])
        self.assertEqual(code, 0)
        self.assertIn("WARNING", out)

    def test_bad_input_returns_nonzero(self):
        main = write_csv(["timestamp,gpu_idle_fraction,step_time_us,iter_latency_us,runnable_tasks",
                          "1,0,1,1,1"])
        code, _, err = self._run([main])
        self.assertEqual(code, 1)
        self.assertIn("Error:", err)


if __name__ == "__main__":
    unittest.main()
