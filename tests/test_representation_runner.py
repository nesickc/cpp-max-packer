import hashlib
import importlib.util
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location(
    "representation_runner", ROOT / "benchmarks" / "run_representation.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def strict_json_harness(text):
    """Independent fake for testing that the runner re-reads retained bytes."""
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate key")
            result[key] = value
        return result

    def finite_float(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError("nonfinite number")
        return parsed

    return json.loads(text, object_pairs_hook=pairs, parse_float=finite_float)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.manifest = self.root / "rc-manifest.json"
        self.expectations = self.root / "import-expectations.json"
        self.generator = self.root / "benchmarks" / "subdivided_cube.py"
        self.generator.parent.mkdir()
        self.manifest.write_text('{"fixture":"manifest"}', encoding="utf-8")
        self.expectations.write_text('{"fixture":"expectations"}', encoding="utf-8")
        self.generator.write_text("# deterministic n=300 generator\n", encoding="utf-8")
        self.checker = self.generator.parent / "representation_checks.py"
        self.validation_runner = self.generator.parent / "run_validation.py"
        self.import_runner = self.generator.parent / "run_import.py"
        for path in (self.checker, self.validation_runner, self.import_runner):
            path.write_text(f"# fake harness {path.name}\n", encoding="utf-8")
        self.sources = []
        self.bindings = []
        for index in range(10):
            path = self.root / "rc" / f"source-{index}.stl"
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(f"source-{index}".encode("ascii"))
            self.sources.append(path)
            self.bindings.append(SimpleNamespace(
                path=path, relative_path=f"rc/source-{index}.stl",
                size=path.stat().st_size, sha256=digest(path)))
        self.large = self.root / "cube-n300.stl"
        self.large.write_bytes(
            b"SpectraPack subdivided cube n=300".ljust(80, b" ")
            + (1_080_000).to_bytes(4, "little") + b"fixture-body")
        self.executable = self.root / "representation.exe"
        self.executable.write_bytes(b"native executable")
        self.revision = "a" * 40
        self.metadata = self.root / "build-metadata.json"
        self.metadata.write_text(json.dumps({
            "build_type": "Release",
            "compiler": "19.51.36252.0",
            "compiler_file_version": "19.51.36252.0",
            "source_revision": self.revision,
        }), encoding="utf-8")
        self.output = self.root / "new-parent" / "qualification.json"
        self.payload = {
            "build": {
                "build_type": "Release", "compiler": "msvc",
                "compiler_version": 195136252,
            },
            "process_peak_working_set_bytes_before": 100,
            "process_peak_working_set_bytes_after": 200,
        }
        self.command = None
        self.artifact_directory = None

    def tearDown(self):
        self.temporary.cleanup()

    def argv(self, output=None):
        return [
            "--executable", str(self.executable),
            "--large-fixture", str(self.large),
            "--build-metadata", str(self.metadata),
            "--output", str(output or self.output),
            "--samples", "3", "--warmup", "1",
            "--timeout-seconds", "7", "--overall-timeout-seconds", "30",
        ]

    def run_main(self, *, stdout=None, mutate=None, before_baseline=None,
                 memory=None, verify=None, output=None):
        stdout = json.dumps(self.payload, separators=(",", ":")) \
            if stdout is None else stdout
        memory = memory or {
            "total_physical_bytes": 16 * 1024**3,
            "available_physical_bytes": 8 * 1024**3,
        }
        verify = verify or mock.Mock(return_value={"checked": True})
        real_capture = runner.capture_identities
        capture_calls = 0

        def capture(paths, deadline):
            nonlocal capture_calls
            if capture_calls == 0 and before_baseline:
                before_baseline()
            capture_calls += 1
            return real_capture(paths, deadline)

        def fake_native(command, timeout, deadline, artifact_directory):
            self.command = command
            self.artifact_directory = Path(artifact_directory)
            self.artifact_directory.joinpath("stdout.txt").write_text(
                stdout, encoding="utf-8")
            self.artifact_directory.joinpath("stderr.txt").write_bytes(b"native log")
            if mutate:
                mutate()
            return json.loads(stdout)

        with (
            mock.patch.object(runner, "ROOT", self.root),
            mock.patch.object(runner, "MANIFEST", self.manifest),
            mock.patch.object(runner, "EXPECTATIONS", self.expectations),
            mock.patch.object(runner, "LARGE_BYTES", self.large.stat().st_size),
            mock.patch.object(runner, "LARGE_SHA256", digest(self.large)),
            mock.patch.object(runner, "load_source_bindings",
                              return_value=self.bindings),
            mock.patch.object(runner, "host_memory_status", return_value=memory),
            mock.patch.object(runner, "repository_metadata", return_value={
                "revision": self.revision, "dirty": True,
            }),
            mock.patch.object(runner, "strict_json", side_effect=strict_json_harness),
            mock.patch.object(runner, "verify_payload", verify),
            mock.patch.object(runner, "capture_identities", side_effect=capture),
            mock.patch.object(runner, "run_native", side_effect=fake_native) as native,
        ):
            result = runner.main(self.argv(output))
        return result, native, verify

    def test_full_orchestration_publishes_bound_provenance_and_resources(self):
        with mock.patch.object(
                runner, "require_bindings_match_identities",
                wraps=runner.require_bindings_match_identities) as binding_guard:
            result, native, verify = self.run_main()

        self.assertEqual(result, 0)
        native.assert_called_once()
        verify.assert_called_once_with(self.payload, self.expectations.resolve(), 3, 1)
        self.assertGreaterEqual(binding_guard.call_count, 2)
        self.assertEqual(self.command[-6:], [
            "--large-fixture", str(self.large.resolve()),
            "--samples", "3", "--warmup", "1",
        ])
        self.assertRegex(
            self.artifact_directory.name,
            r"^qualification\.json\.artifacts-[0-9a-f]{32}$")
        report = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertTrue(report["qualified"])
        self.assertEqual(report["metadata"]["repository"], {
            "revision": self.revision, "dirty": True,
        })
        self.assertEqual(report["metadata"]["parameters"]["generator"], {
            "subdivisions": 300,
        })
        self.assertEqual(report["metadata"]["artifact_directory"],
                         str(self.artifact_directory))
        self.assertEqual(report["resources"]["host_physical_memory"], {
            "total_physical_bytes": 16 * 1024**3,
            "available_physical_bytes": 8 * 1024**3,
        })
        self.assertEqual(report["resources"]["process_peak_working_set_bytes"], {
            "before": 100, "after": 200,
        })
        self.assertEqual(len(report["hashes"]["sources"]), 10)
        self.assertEqual(report["hashes"]["large_fixture"]["sha256"],
                         digest(self.large))
        self.assertEqual(report["hashes"]["generator"]["sha256"],
                         digest(self.generator))
        self.assertEqual(set(report["hashes"]["harness"]), {
            "run_representation.py", "representation_checks.py",
            "run_validation.py", "run_import.py",
        })
        self.assertEqual(report["performance"], {"checked": True})

    def test_large_replacement_before_baseline_cannot_rebind_pinned_fixture(self):
        output = self.root / "large-race.json"
        output.write_bytes(b"prior report")

        def replace_large():
            changed = bytearray(self.large.read_bytes())
            changed[-1] ^= 1
            self.large.write_bytes(changed)

        result, native, _ = self.run_main(
            before_baseline=replace_large, output=output)
        self.assertEqual(result, 2)
        native.assert_not_called()
        self.assertEqual(output.read_bytes(), b"prior report")

    def test_metadata_replacement_between_old_parse_and_baseline_is_rejected(self):
        output = self.root / "metadata-race.json"
        output.write_bytes(b"prior report")
        result, native, _ = self.run_main(
            before_baseline=lambda: self.metadata.write_bytes(b"not JSON"),
            output=output)
        self.assertEqual(result, 2)
        native.assert_not_called()
        self.assertEqual(output.read_bytes(), b"prior report")

    def test_checker_mutation_retains_prior_report_and_raw_process_artifacts(self):
        output = self.root / "checker-race.json"
        output.write_bytes(b"prior report")
        result, native, _ = self.run_main(
            mutate=lambda: self.checker.write_bytes(b"changed checker"),
            output=output)
        self.assertEqual(result, 2)
        native.assert_called_once()
        self.assertEqual(output.read_bytes(), b"prior report")
        self.assertEqual(
            self.artifact_directory.joinpath("stdout.txt").read_text(encoding="utf-8"),
            json.dumps(self.payload, separators=(",", ":")))
        self.assertEqual(
            self.artifact_directory.joinpath("stderr.txt").read_bytes(),
            b"native log")

    def test_retained_stdout_is_strictly_reparsed_before_payload_checks(self):
        corrupt = [
            '{"value":1,"value":2}',
            '{"value":1e999}',
        ]
        for index, stdout in enumerate(corrupt):
            with self.subTest(stdout=stdout):
                output = self.root / f"corrupt-{index}.json"
                result, _, verify = self.run_main(stdout=stdout, output=output)
                self.assertEqual(result, 2)
                verify.assert_not_called()
                self.assertFalse(output.exists())

        def reject_boolean(payload, _expectations, _samples, _warmup):
            if type(payload.get("value")) is bool:
                raise ValueError("boolean is not numeric")
            return {}

        output = self.root / "boolean.json"
        result, _, _ = self.run_main(
            stdout='{"value":true}', verify=mock.Mock(side_effect=reject_boolean),
            output=output)
        self.assertEqual(result, 2)
        self.assertFalse(output.exists())

    def test_every_protected_input_is_rechecked_after_native_process(self):
        protected = [
            *self.sources, self.manifest, self.expectations, self.generator,
            self.large, self.executable, self.metadata,
        ]
        for index, target in enumerate(protected):
            with self.subTest(target=target.name):
                original = target.read_bytes()
                output = self.root / f"mutated-{index}.json"
                result, _, _ = self.run_main(
                    mutate=lambda path=target: path.write_bytes(
                        path.read_bytes() + b"mutated"),
                    output=output)
                self.assertEqual(result, 2)
                self.assertFalse(output.exists())
                target.write_bytes(original)

    def test_output_alias_and_insufficient_memory_do_not_launch_native(self):
        original = self.manifest.read_bytes()
        result, native, _ = self.run_main(output=self.manifest)
        self.assertEqual(result, 2)
        native.assert_not_called()
        self.assertEqual(self.manifest.read_bytes(), original)

        result, native, _ = self.run_main(memory={
            "total_physical_bytes": 8 * 1024**3,
            "available_physical_bytes": 4 * 1024**3 - 1,
        })
        self.assertEqual(result, 2)
        native.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_large_fixture_header_compiler_and_revision_are_bound(self):
        original_large = self.large.read_bytes()
        bad_header = bytearray(original_large)
        bad_header[80:84] = (1_079_999).to_bytes(4, "little")
        self.large.write_bytes(bad_header)
        result, native, _ = self.run_main()
        self.assertEqual(result, 2)
        native.assert_not_called()
        self.large.write_bytes(original_large)

        for field, value in (
            ("compiler", "19.50.36252.0"),
            ("source_revision", "b" * 40),
        ):
            with self.subTest(field=field):
                metadata = json.loads(self.metadata.read_text(encoding="utf-8"))
                original = metadata[field]
                metadata[field] = value
                self.metadata.write_text(json.dumps(metadata), encoding="utf-8")
                result, native, _ = self.run_main()
                self.assertEqual(result, 2)
                if field == "source_revision":
                    native.assert_not_called()
                self.assertFalse(self.output.exists())
                metadata[field] = original
                self.metadata.write_text(json.dumps(metadata), encoding="utf-8")

        self.payload["build"]["compiler_version"] = 195036252
        output = self.root / "native-compiler-mismatch.json"
        result, native, _ = self.run_main(output=output)
        self.assertEqual(result, 2)
        native.assert_called_once()
        self.assertFalse(output.exists())

    def test_cli_rejects_nonfinite_and_out_of_bounds_numbers(self):
        valid = self.argv()
        self.assertEqual((runner.parse_arguments(valid).samples,
                          runner.parse_arguments(valid).warmup), (3, 1))
        for option, value in (
            ("--samples", "2"), ("--warmup", "0"),
            ("--timeout-seconds", "nan"),
            ("--overall-timeout-seconds", "inf"),
        ):
            with self.subTest(option=option, value=value), self.assertRaises(SystemExit):
                args = list(valid)
                args[args.index(option) + 1] = value
                runner.parse_arguments(args)
        with self.assertRaises(SystemExit):
            runner.parse_arguments(valid + ["--artifact-directory", "anything"])

    def test_direct_script_import_path_supports_help(self):
        completed = subprocess.run(
            [sys.executable, str(ROOT / "benchmarks" / "run_representation.py"),
             "--help"],
            cwd=self.root, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("--large-fixture", completed.stdout)

    @unittest.skipUnless(sys.platform == "win32", "Windows-only qualifier")
    def test_live_host_memory_measurement_is_physical_memory(self):
        status = runner.host_memory_status()
        self.assertGreater(status["total_physical_bytes"], 0)
        self.assertGreater(status["available_physical_bytes"], 0)
        self.assertLessEqual(status["available_physical_bytes"],
                             status["total_physical_bytes"])


if __name__ == "__main__":
    unittest.main()
