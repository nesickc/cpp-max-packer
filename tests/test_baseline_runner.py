import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location(
    "baseline_runner", ROOT / "benchmarks" / "run_baseline.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)
from tests.test_baseline_benchmark import complete_payload


class BaselineRunnerTests(unittest.TestCase):
    """QA-01 runner tests keep real process, verifier and publication paths."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        fixture_root = ROOT / "tests" / "fixtures"
        self.manifest = self.root / "tests/fixtures/rc-manifest.json"
        self.expectations = self.root / "tests/fixtures/import-expectations.json"
        self.manifest.parent.mkdir(parents=True)
        self.manifest.write_bytes((fixture_root / "rc-manifest.json").read_bytes())
        self.expectations.write_bytes(
            (fixture_root / "import-expectations.json").read_bytes())

        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.sources = []
        for record in manifest["records"]:
            source = self.root / record["path"]
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes((ROOT / record["path"]).read_bytes())
            self.sources.append(source)

        benchmark_root = self.root / "benchmarks"
        benchmark_root.mkdir()
        self.harness = []
        for name in ("run_baseline.py", "baseline_checks.py", "run_validation.py",
                     "representation_checks.py", "run_import.py", "run_representation.py"):
            target = benchmark_root / name
            target.write_bytes((ROOT / "benchmarks" / name).read_bytes())
            self.harness.append(target)

        self.executable = self.root / "baseline.exe"
        self.executable.write_bytes(b"native baseline executable")
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
            text=True, check=True)
        self.revision = completed.stdout.strip().lower()
        self.repository = {"revision": self.revision, "dirty": False}
        self.metadata = self.root / "build-metadata.json"
        self.metadata.write_text(json.dumps({
            "build_type": "Release", "compiler": "19.51.36252.0",
            "compiler_file_version": "19.51.36252.0",
            "source_revision": self.revision,
        }), encoding="utf-8")
        self.output = self.root / "new-parent" / "qualification.json"
        self.payload = complete_payload(
            expectations_path=self.expectations, manifest_path=self.manifest,
            source_root=self.root)
        self.payload_path = self.root / "complete-payload.json"
        self.write_payload()

    def tearDown(self):
        self.temporary.cleanup()

    def write_payload(self):
        self.payload_path.write_text(
            json.dumps(self.payload, separators=(",", ":")), encoding="utf-8")

    def argv(self, output=None, timeout="10", overall="60", pilot=False):
        result = [
            "--executable", str(self.executable),
            "--build-metadata", str(self.metadata),
            "--output", str(output or self.output),
            "--samples", "3", "--warmup", "1",
            "--timeout-seconds", timeout,
            "--overall-timeout-seconds", overall,
        ]
        if pilot:
            result.append("--pilot")
        return result

    def child(self, body):
        script = self.root / f"child-{len(list(self.root.glob('child-*.py')))}.py"
        script.write_text(body, encoding="utf-8")
        return script

    def payload_child(self, mutate=None, delay=0):
        lines = ["import time", "from pathlib import Path"]
        if delay:
            lines.append(f"time.sleep({delay!r})")
        if mutate is not None:
            lines.append(
                f"p=Path({str(mutate)!r});p.write_bytes(p.read_bytes()+b' mutation')")
        lines.append(
            f"print(Path({str(self.payload_path)!r}).read_text(encoding='utf-8'))")
        return self.child("\n".join(lines) + "\n")

    def run_main(self, child, *, output=None, timeout="10", overall="60",
                 pilot=False, repository=None, allow_deadline_error=False):
        command = [sys.executable, str(child)]
        with (
            mock.patch.object(runner, "ROOT", self.root),
            mock.patch.object(runner, "MANIFEST", self.manifest),
            mock.patch.object(runner, "EXPECTATIONS", self.expectations),
            mock.patch.object(runner, "repository_metadata",
                              return_value=repository or self.repository),
            mock.patch.object(runner, "build_native_command",
                              return_value=command) as builder,
        ):
            diagnostics = io.StringIO()
            with contextlib.redirect_stderr(diagnostics):
                result = runner.main(self.argv(output, timeout, overall, pilot))
        self.last_stderr = diagnostics.getvalue()
        if not allow_deadline_error:
            self.assertNotIn("validation benchmark watchdog expired",
                             self.last_stderr)
            self.assertNotIn("overall validation qualification deadline expired",
                             self.last_stderr)
        return result, builder

    def test_complete_child_runs_real_checker_and_publishes_bound_report(self):
        runs = self.payload["workloads"][0]["runs"]
        for run, search, fresh in zip(runs[1:], (9.0, 1.0, 4.0),
                                      (3.0, 8.0, 2.0)):
            run["search_ms"] = search
            run["fresh_revalidation_ms"] = fresh
        self.write_payload()
        result, builder = self.run_main(self.payload_child())
        self.assertEqual(result, 0, self.last_stderr)
        builder.assert_called_once()
        report = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertTrue(report["qualified"])
        self.assertEqual(report["metadata"]["repository"], self.repository)
        self.assertEqual(report["payload"], self.payload)
        first_timing = report["performance"]["workloads"][0]
        self.assertEqual(first_timing, {
            "id": "cube_exact",
            "search": {"samples_ms": [9.0, 1.0, 4.0],
                       "median_ms": 4.0, "p95_ms": 9.0},
            "fresh_revalidation": {"samples_ms": [3.0, 8.0, 2.0],
                                   "median_ms": 3.0, "p95_ms": 8.0},
        })
        artifacts = list(self.output.parent.glob(self.output.name + ".artifacts-*"))
        self.assertEqual(len(artifacts), 1)
        self.assertEqual(json.loads((artifacts[0] / "stdout.txt").read_text(
            encoding="utf-8")), self.payload)

    def test_malformed_incomplete_and_pilot_output_preserve_prior_report(self):
        cases = [
            (self.child("print('{bad json}')\n"), False),
            (self.child("print('{\"workloads\":[]}')\n"), False),
        ]
        pilot_payload = json.loads(self.payload_path.read_text(encoding="utf-8"))
        pilot_payload["pilot"] = True
        pilot_file = self.root / "pilot-payload.json"
        pilot_file.write_text(json.dumps(pilot_payload), encoding="utf-8")
        cases.append((self.child(
            f"from pathlib import Path\nprint(Path({str(pilot_file)!r}).read_text())\n"),
                      True))
        for index, (child, pilot) in enumerate(cases):
            with self.subTest(index=index):
                self.output.parent.mkdir(parents=True, exist_ok=True)
                self.output.write_bytes(b"prior report")
                result, _ = self.run_main(child, pilot=pilot)
                self.assertEqual(result, 2)
                self.assertEqual(self.output.read_bytes(), b"prior report")

    def test_output_alias_is_rejected_before_child_launch(self):
        marker = self.root / "launched"
        child = self.child(
            f"from pathlib import Path\nPath({str(marker)!r}).write_text('yes')\n")
        original = self.manifest.read_bytes()
        result, _ = self.run_main(child, output=self.manifest)
        self.assertEqual(result, 2)
        self.assertFalse(marker.exists())
        self.assertEqual(self.manifest.read_bytes(), original)

    def test_every_registered_or_harness_input_is_rechecked_after_child(self):
        protected = [*self.sources, self.manifest, self.expectations,
                     self.executable, self.metadata, *self.harness]
        for index, target in enumerate(protected):
            with self.subTest(target=target):
                original = target.read_bytes()
                output = self.root / f"mutated-{index}.json"
                output.write_bytes(b"prior report")
                result, _ = self.run_main(self.payload_child(mutate=target),
                                          output=output)
                self.assertEqual(result, 2)
                self.assertTrue(self.last_stderr.strip())
                self.assertEqual(output.read_bytes(), b"prior report")
                target.write_bytes(original)

    def test_child_and_whole_harness_deadlines_leave_no_partial_success(self):
        for timeout, overall in (("0.02", "2"), ("2", "0.02")):
            with self.subTest(timeout=timeout, overall=overall):
                output = self.root / f"deadline-{timeout}-{overall}.json"
                output.write_bytes(b"prior report")
                started = time.monotonic()
                result, _ = self.run_main(self.payload_child(delay=5), output=output,
                                          timeout=timeout, overall=overall,
                                          allow_deadline_error=True)
                self.assertEqual(result, 2)
                expected = ("validation benchmark watchdog expired"
                            if timeout == "0.02" else
                            "overall validation qualification deadline expired")
                self.assertIn(expected, self.last_stderr)
                self.assertLess(time.monotonic() - started, 2.0)
                self.assertEqual(output.read_bytes(), b"prior report")

    def test_build_revision_mismatch_is_rejected_before_launch(self):
        metadata = json.loads(self.metadata.read_text(encoding="utf-8"))
        metadata["source_revision"] = "b" * 40
        self.metadata.write_text(json.dumps(metadata), encoding="utf-8")
        marker = self.root / "launched-for-mismatch"
        child = self.child(
            f"from pathlib import Path\nPath({str(marker)!r}).write_text('yes')\n")
        result, _ = self.run_main(child)
        self.assertEqual(result, 2)
        self.assertFalse(marker.exists())
        self.assertFalse(self.output.exists())

    def test_dirty_repository_is_rejected_before_launch_and_preserves_report(self):
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(b"prior report")
        marker = self.root / "launched-for-dirty-tree"
        child = self.child(
            f"from pathlib import Path\nPath({str(marker)!r}).write_text('yes')\n")
        dirty = {"revision": self.revision, "dirty": True}

        result, builder = self.run_main(child, repository=dirty)

        self.assertEqual(result, 2)
        builder.assert_not_called()
        self.assertFalse(marker.exists())
        self.assertEqual(self.output.read_bytes(), b"prior report")

    def test_invalid_argument_numbers_and_direct_help_are_available(self):
        for flag, value in (("--timeout-seconds", "nan"),
                            ("--overall-timeout-seconds", "inf"),
                            ("--samples", "2"), ("--warmup", "0")):
            with self.subTest(flag=flag), self.assertRaises(SystemExit):
                arguments = self.argv()
                arguments[arguments.index(flag) + 1] = value
                runner.parse_arguments(arguments)
        completed = subprocess.run(
            [sys.executable, str(ROOT / "benchmarks" / "run_baseline.py"), "--help"],
            cwd=self.root, capture_output=True, text=True, timeout=5, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("--overall-timeout-seconds", completed.stdout)


if __name__ == "__main__":
    unittest.main()
