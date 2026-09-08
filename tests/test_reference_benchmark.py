import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
RUNNER = ROOT / "benchmarks" / "run_reference.py"
PYTHON = sys.executable


def payload(*, samples=(1, 2, 3, 4, 5), compiler="MSVC 195136252", build_type="Release", workloads=None, schema_version=1):
    if workloads is None:
        workloads = [{"name": "reference", "work_units": 12, "samples_ms": list(samples), "checksum": "fixture-v1"}]
    return {"schema_version": schema_version, "benchmark_kind": "test_reference_oracle", "compiler": compiler, "build_type": build_type, "workloads": workloads}


class ReferenceBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def fake_payload(self, value, name="native.py"):
        executable = self.directory / name
        executable.write_text("print(" + repr(json.dumps(value, allow_nan=True)) + ")\n", encoding="utf-8")
        return executable

    def fake_source(self, source, name="native.py"):
        executable = self.directory / name
        executable.write_text(source, encoding="utf-8")
        return executable

    def execute(self, executable, *arguments, output=None, cwd=None, executable_argument=None):
        report = output or self.directory / "report.json"
        process = subprocess.run([PYTHON, str(RUNNER), "--executable", executable_argument or str(executable), "--output", str(report), *arguments], cwd=cwd, capture_output=True, text=True, timeout=10)
        return process, report

    def create_baseline(self, value=None):
        process, baseline = self.execute(self.fake_payload(value or payload()), "--samples", "5")
        self.assertEqual(process.returncode, 0, process.stderr)
        return baseline

    def test_statistics_and_schema(self):
        process, report = self.execute(self.fake_payload(payload()), "--samples", "5")
        self.assertEqual(process.returncode, 0, process.stderr)
        workload = json.loads(report.read_text())["workloads"][0]
        self.assertEqual(workload["median_ms"], 3)
        self.assertEqual(workload["min_ms"], 1)
        self.assertEqual(workload["p95_ms"], 5)
        metadata = json.loads(report.read_text())["metadata"]
        self.assertIn("git_revision", metadata)
        self.assertIn("git_dirty", metadata)
        self.assertRegex(metadata["executable_sha256"], r"^[0-9a-f]{64}$")

    def test_relative_executable_from_another_cwd_is_hashed_and_launched(self):
        relative_directory = self.directory / "relative"
        relative_directory.mkdir()
        executable = self.fake_payload(payload(), "relative/native.py")
        process, _ = self.execute(executable, "--samples", "5", cwd=self.directory,
                                  executable_argument="relative/native.py")
        self.assertEqual(process.returncode, 0, process.stderr)

    def test_timeout_malformed_output_and_invalid_parameters_fail_cleanly(self):
        cases = [(self.fake_source("import sys; sys.exit(2)", "exit.py"), ()), (self.fake_source("print('not json')", "malformed.py"), ()), (self.fake_source("import time; time.sleep(2)", "timeout.py"), ("--timeout-seconds", "0.01")), (self.fake_payload(payload(), "timeout-parameter.py"), ("--timeout-seconds", "nan")), (self.fake_payload(payload(), "tolerance-parameter.py"), ("--max-regression-percent", "-1"))]
        for executable, arguments in cases:
            with self.subTest(arguments=arguments):
                process, _ = self.execute(executable, *arguments)
                self.assertEqual(process.returncode, 2)
                self.assertNotIn("Traceback", process.stderr)

    def test_strict_native_payload_validation(self):
        invalid = [payload(schema_version=True), payload(schema_version=1.0), payload(compiler=""), payload(build_type=""), payload(workloads=7), payload(workloads=[{"name": "reference", "work_units": 1.5, "samples_ms": [1, 2, 3, 4, 5], "checksum": "x"}]), payload(workloads=[{"name": "reference", "work_units": 1, "samples_ms": [1, float("inf"), 3, 4, 5], "checksum": "x"}]), payload(workloads=[{"name": "reference", "work_units": 1, "samples_ms": [10 ** 400, 2, 3, 4, 5], "checksum": "x"}])]
        for index, native in enumerate(invalid):
            with self.subTest(index=index):
                process, _ = self.execute(self.fake_payload(native, f"invalid-{index}.py"))
                self.assertEqual(process.returncode, 2)
                self.assertNotIn("Traceback", process.stderr)

    def test_baseline_recomputes_cached_statistics_and_writes_regression_report(self):
        baseline = self.create_baseline(payload(samples=(10, 10, 10, 10, 10)))
        forged = json.loads(baseline.read_text())
        forged["workloads"][0]["median_ms"] = float("nan")
        baseline.write_text(json.dumps(forged, allow_nan=True), encoding="utf-8")
        process, report = self.execute(self.fake_payload(payload(samples=(13, 13, 13, 13, 13))), "--baseline", str(baseline), "--max-regression-percent", "20", output=self.directory / "regression.json")
        self.assertEqual(process.returncode, 2)
        self.assertTrue(report.exists(), "measured regression should retain its new report")
        self.assertNotIn("Traceback", process.stderr)

    def test_baseline_output_alias_is_rejected_without_overwrite(self):
        baseline = self.create_baseline()
        before = baseline.read_text(encoding="utf-8")
        process, _ = self.execute(self.fake_payload(payload(samples=(100,) * 5)), "--baseline", str(baseline), output=baseline)
        self.assertEqual(process.returncode, 2)
        self.assertEqual(baseline.read_text(encoding="utf-8"), before)

    def test_invalid_baseline_shapes_and_compatibility_mismatches_fail_cleanly(self):
        for index, invalid in enumerate([[], {"schema_version": 1, "benchmark_kind": "test_reference_oracle", "metadata": [], "workloads": []}]):
            baseline = self.directory / f"invalid-baseline-{index}.json"
            baseline.write_text(json.dumps(invalid), encoding="utf-8")
            process, _ = self.execute(self.fake_payload(payload()), "--baseline", str(baseline))
            self.assertEqual(process.returncode, 2)
            self.assertNotIn("Traceback", process.stderr)
        baseline = self.create_baseline()
        forged = json.loads(baseline.read_text())
        forged["workloads"][0]["samples_ms"][0] = 10 ** 400
        baseline.write_text(json.dumps(forged), encoding="utf-8")
        process, _ = self.execute(self.fake_payload(payload(), "huge-baseline.py"), "--baseline", str(baseline), output=self.directory / "huge-baseline-candidate.json")
        self.assertEqual(process.returncode, 2)
        self.assertNotIn("Traceback", process.stderr)

    def test_compatible_baseline_succeeds_and_each_mismatch_reaches_its_diagnostic(self):
        baseline = self.create_baseline()
        process, _ = self.execute(self.fake_payload(payload(), "matching.py"), "--baseline", str(baseline), output=self.directory / "matching-candidate.json")
        self.assertEqual(process.returncode, 0, process.stderr)

        def check_mutation(name, mutate, expected):
            baseline_path = self.create_baseline()
            altered = json.loads(baseline_path.read_text())
            mutate(altered)
            baseline_path.write_text(json.dumps(altered), encoding="utf-8")
            process, _ = self.execute(self.fake_payload(payload(), f"{name}.py"), "--baseline", str(baseline_path), output=self.directory / f"{name}-candidate.json")
            self.assertEqual(process.returncode, 2)
            self.assertIn(expected, process.stderr)
            self.assertNotIn("baseline and output", process.stderr)

        check_mutation("compiler", lambda report: report["metadata"]["native"].update(compiler="other"), "incompatible baseline native build")
        check_mutation("parameters", lambda report: report["metadata"]["parameters"].update(warmup=2), "incompatible baseline parameters")
        check_mutation("host", lambda report: report["metadata"].update(os="other"), "incompatible baseline host")
        check_mutation("workload", lambda report: report["workloads"][0].update(checksum="other"), "incompatible baseline workloads")


if __name__ == "__main__":
    unittest.main()
