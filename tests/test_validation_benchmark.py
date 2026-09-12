import copy
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location(
    "run_validation", ROOT / "benchmarks" / "run_validation.py")
ITEMS = [
    "rc/items/pryanik_1.STL",
    "rc/items/pryanik_2.STL",
    "rc/items/ulamok_2kg_simplified.stl",
]
CONTAINERS = [
    "rc/containers/10_kg_np.stl",
    "rc/containers/15_kg_np_long.stl",
    "rc/containers/20_kg_np.stl",
    "rc/containers/30_kg_np.stl",
    "rc/containers/30_kg_np_cubic.stl",
    "rc/containers/5_kg_np.stl",
]
CHECK_NAMES = [
    "input", "orientation", "broad_phase", "pair_solids",
    "containment", "clearance",
]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def independent_workloads(expectations_path):
    document = json.loads(Path(expectations_path).read_text(encoding="utf-8"))
    bounds = {
        record["path"]: record["expected"]["mesh_bounds_mm"]
        for record in document["records"]
        if record["expected"]["state"] == "accepted"
    }
    workloads = []
    quaternion = [0.0, 0.0, 0.0, 1.0]
    for object_path in ITEMS:
        for container_path in CONTAINERS:
            object_bounds = bounds[object_path]
            container_bounds = bounds[container_path]
            object_min = object_bounds["min"]
            object_max = object_bounds["max"]
            container_min = container_bounds["min"]
            container_max = container_bounds["max"]
            center = [
                (container_min[axis] + container_max[axis]
                 - object_min[axis] - object_max[axis]) / 2.0
                for axis in range(3)
            ]
            first = center.copy()
            second = center.copy()
            first[1] = (container_min[1]
                        + (container_max[1] - container_min[1]) / 3.0
                        - (object_min[1] + object_max[1]) / 2.0)
            second[1] = (container_min[1]
                         + 2.0 * (container_max[1] - container_min[1]) / 3.0
                         - (object_min[1] + object_max[1]) / 2.0)
            outside = center.copy()
            outside[0] = container_min[0] - object_max[0] - 1.0
            short_gap = center.copy()
            short_gap[0] = container_min[0] - object_min[0] + 0.5

            def pose0(position):
                return {
                    "copy_id": "copy-0",
                    "translation_mm": position,
                    "quaternion_xyzw": quaternion.copy(),
                }

            cases = [
                ("centered_single", [pose0(center)], "valid"),
                ("separated_pair", [pose0(first), {
                    "copy_id": "copy-1", "translation_mm": second,
                    "quaternion_xyzw": quaternion.copy()}], "valid"),
                ("coincident_pair", [pose0(center), {
                    "copy_id": "copy-1", "translation_mm": center.copy(),
                    "quaternion_xyzw": quaternion.copy()}], "invalid"),
                ("outside_min_x", [pose0(outside)], "invalid"),
                ("short_wall_gap", [pose0(short_gap)], "invalid"),
            ]
            for case, poses, verdict in cases:
                workloads.append((case, object_path, container_path, poses, verdict))

    poses = []
    for z_index in range(4):
        for y_index in range(4):
            for x_index in range(4):
                poses.append({
                    "copy_id": f"copy-{len(poses)}",
                    "translation_mm": [
                        2.0 + 3.0 * x_index,
                        2.0 + 3.0 * y_index,
                        2.0 + 3.0 * z_index,
                    ],
                    "quaternion_xyzw": quaternion.copy(),
                })
    workloads.append(("analytic_separated_64", "analytic_unit_cube",
                      "analytic_box_14", poses, "valid"))
    if len(workloads) != 91:
        raise AssertionError("independent fixture must contain 91 workloads")
    return workloads


def checks_for(case, verdict):
    if verdict == "valid":
        states = ["complete"] * 6
    elif case == "coincident_pair":
        states = ["complete", "complete", "not_run", "complete",
                  "not_run", "not_run"]
    else:
        states = ["complete", "complete", "complete", "complete",
                  "complete", "not_run"]
    return [
        {"check": name, "state": state, "method": f"fixture-{name}"}
        for name, state in zip(CHECK_NAMES, states)
    ]


def report_for(case, verdict, poses, work_index):
    if verdict == "valid":
        affected = []
    elif case == "coincident_pair":
        affected = ["copy-1", "copy-0"]
    else:
        affected = ["copy-0"]
    return {
        "status": verdict,
        "code": "VALID" if verdict == "valid" else "FIXTURE_INVALID",
        "message": "fixture validation result",
        "epsilon_mm": 1e-6,
        "kernel_revision": "fixture-kernel-v1",
        "aabb_pair_tests": len(poses) * (len(poses) - 1) // 2,
        "kernel_work": 1000 + work_index,
        "working_bytes_peak": 4096,
        "affected_copy_ids": affected,
        "affected_ids_truncated": False,
        "checks": checks_for(case, verdict),
    }


def positive_payload(expectations_path, samples=3, warmup=1):
    workloads = []
    for index, (case, object_path, container_path, poses, verdict) in enumerate(
            independent_workloads(expectations_path)):
        report = report_for(case, verdict, poses, index)
        runs = []
        for run_index in range(warmup + samples):
            runs.append({
                "phase": "warmup" if run_index < warmup else "sample",
                "elapsed_ms": float(index + run_index + 1),
                "report": copy.deepcopy(report),
                "has_validated_solution": verdict == "valid",
                "validated_copy_count": len(poses) if verdict == "valid" else 0,
            })
        workloads.append({
            "case": case,
            "object_path": object_path,
            "container_path": container_path,
            "poses": copy.deepcopy(poses),
            "constraints": {
                "pair_clearance_mm": 1.0,
                "wall_clearance_mm": 1.0,
            },
            "runs": runs,
        })
    return {
        "schema_version": 1,
        "benchmark_kind": "native_validation",
        "compiler": "MSVC fixture",
        "build_type": "Release",
        "parameters": {"samples": samples, "warmup": warmup},
        "setup_ms": 2.5,
        "workloads": workloads,
    }


class ValidationHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = importlib.util.module_from_spec(SPEC)
        SPEC.loader.exec_module(cls.module)
        cls.expectations = ROOT / "tests" / "fixtures" / "import-expectations.json"

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def payload(self):
        return positive_payload(self.expectations)

    def assert_rejected(self, mutate):
        payload = self.payload()
        mutate(payload)
        with self.assertRaises(self.module.HarnessError):
            self.module.verify_payload(payload, self.expectations, 3, 1)

    def test_complete_independent_91_case_fixture_qualifies(self):
        payload = self.payload()
        verified = self.module.verify_payload(payload, self.expectations, 3, 1)
        self.assertEqual(len(verified), 91)

    def test_one_field_matrix_pose_verdict_and_run_mutations_reject(self):
        mutations = {
            "forged case": lambda value: value["workloads"][0].__setitem__("case", "forged"),
            "wrong pose": lambda value: value["workloads"][0]["poses"][0]["translation_mm"].__setitem__(0, 999.0),
            "wrong verdict": lambda value: value["workloads"][0]["runs"][0]["report"].__setitem__("status", "invalid"),
            "indeterminate invalid": lambda value: value["workloads"][2]["runs"][0]["report"].__setitem__("status", "indeterminate"),
            "warmup count": lambda value: value["workloads"][0]["runs"].pop(0),
            "changed work": lambda value: value["workloads"][0]["runs"][1]["report"].__setitem__("kernel_work", 999999),
            "missing affected ids": lambda value: value["workloads"][2]["runs"][0]["report"].pop("affected_copy_ids"),
            "unknown affected id": lambda value: value["workloads"][2]["runs"][0]["report"].__setitem__("affected_copy_ids", ["forged-copy"]),
            "missing check": lambda value: value["workloads"][2]["runs"][0]["report"]["checks"].pop(),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                self.assert_rejected(mutation)

    def test_boolean_values_are_never_accepted_as_numbers(self):
        mutations = {
            "schema": lambda value: value.__setitem__("schema_version", True),
            "samples": lambda value: value["parameters"].__setitem__("samples", True),
            "warmup": lambda value: value["parameters"].__setitem__("warmup", True),
            "setup": lambda value: value.__setitem__("setup_ms", False),
            "clearance": lambda value: value["workloads"][0]["constraints"].__setitem__("pair_clearance_mm", True),
            "elapsed": lambda value: value["workloads"][0]["runs"][0].__setitem__("elapsed_ms", True),
            "count": lambda value: value["workloads"][0]["runs"][0]["report"].__setitem__("kernel_work", True),
            "snapshot count": lambda value: value["workloads"][0]["runs"][0].__setitem__("validated_copy_count", True),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                self.assert_rejected(mutation)

    def test_strict_shapes_enums_flags_and_invalid_states(self):
        mutations = {
            "empty compiler": lambda value: value.__setitem__("compiler", ""),
            "extra top field": lambda value: value.__setitem__("expected", True),
            "bad report flag": lambda value: value["workloads"][2]["runs"][0]["report"].__setitem__("affected_ids_truncated", "yes"),
            "truncated ids": lambda value: value["workloads"][2]["runs"][0]["report"].__setitem__("affected_ids_truncated", True),
            "bad check state": lambda value: value["workloads"][2]["runs"][0]["report"]["checks"][0].__setitem__("state", "unknown"),
            "incomplete invalid check": lambda value: value["workloads"][2]["runs"][0]["report"]["checks"][3].__setitem__("state", "not_run"),
            "invalid snapshot": lambda value: value["workloads"][2]["runs"][0].__setitem__("has_validated_solution", True),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                self.assert_rejected(mutation)

    def create_source_fixture(self):
        root = self.directory / "fixture-root"
        root.mkdir()
        records = []
        for index in range(10):
            relative = f"rc/source-{index}.stl"
            source = root / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(f"source-{index}".encode("ascii"))
            records.append({
                "path": relative,
                "role": "container" if index < 6 else "item",
                "bytes": source.stat().st_size,
                "sha256": sha256(source),
            })
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({"schema_version": 1, "records": records}),
                            encoding="utf-8")
        expectations = root / "expectations.json"
        expectations.write_text(json.dumps({
            "schema_version": 1,
            "manifest_sha256": sha256(manifest),
            "records": [{"path": record["path"], "expected": {}}
                        for record in records],
        }), encoding="utf-8")
        return root, manifest, expectations, records

    def test_all_ten_manifest_sources_and_expectation_binding_are_checked(self):
        root, manifest, expectations, records = self.create_source_fixture()
        bindings = self.module.load_source_bindings(root, manifest, expectations)
        self.assertEqual(len(bindings), 10)

        changed = root / records[-1]["path"]
        changed.write_bytes(b"tamper-9")
        with self.assertRaises(self.module.HarnessError):
            self.module.load_source_bindings(root, manifest, expectations)

        changed.write_bytes(b"source-9")
        changed.write_bytes(changed.read_bytes() + b"changed-size")
        with self.assertRaises(self.module.HarnessError):
            self.module.load_source_bindings(root, manifest, expectations)

        changed.write_bytes(b"source-9")
        document = json.loads(expectations.read_text(encoding="utf-8"))
        document["manifest_sha256"] = "0" * 64
        expectations.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaises(self.module.HarnessError):
            self.module.load_source_bindings(root, manifest, expectations)

    def test_source_change_between_binding_load_and_baseline_is_rejected(self):
        root, manifest, expectations, records = self.create_source_fixture()
        bindings = self.module.load_source_bindings(root, manifest, expectations)
        changed = root / records[0]["path"]
        changed.write_bytes(b"tamper-0")  # Same byte count as the pinned source.
        identities = self.module.capture_identities(
            [binding.path for binding in bindings], float("inf"))
        with self.assertRaises(self.module.HarnessError):
            self.module.require_bindings_match_identities(bindings, identities)

    def test_output_alias_protection_covers_every_source_hardlink(self):
        root, manifest, expectations, _ = self.create_source_fixture()
        bindings = self.module.load_source_bindings(root, manifest, expectations)
        for index, binding in enumerate(bindings):
            alias = self.directory / f"alias-{index}.json"
            os.link(binding.path, alias)
            with self.subTest(index=index), self.assertRaises(self.module.HarnessError):
                self.module.ensure_distinct_output(alias, [item.path for item in bindings])

    def test_hardlink_protection_also_covers_harness_and_build_inputs(self):
        protected = []
        for name in ("benchmark.exe", "build.json", "manifest.json",
                     "expectations.json"):
            path = self.directory / name
            path.write_bytes(name.encode("ascii"))
            protected.append(path)
        for index, path in enumerate(protected):
            alias = self.directory / f"protected-alias-{index}.json"
            os.link(path, alias)
            with self.subTest(path=str(path)), self.assertRaises(self.module.HarnessError):
                self.module.ensure_distinct_output(alias, protected)

    def test_child_timeout_retains_partial_raw_streams(self):
        child = self.directory / "child.py"
        child.write_text(
            "import sys,time\n"
            "sys.stdout.buffer.write(b'partial-out\\xff'); sys.stdout.flush()\n"
            "sys.stderr.buffer.write(b'partial-err\\xfe'); sys.stderr.flush()\n"
            "time.sleep(5)\n", encoding="utf-8")
        artifact = self.directory / "artifacts"
        artifact.mkdir()
        with self.assertRaises(self.module.HarnessError):
            self.module.run_native(
                [sys.executable, str(child)], 0.05,
                time.monotonic() + 2.0, artifact)
        self.assertIn(b"partial-out\xff", (artifact / "stdout.txt").read_bytes())
        self.assertIn(b"partial-err\xfe", (artifact / "stderr.txt").read_bytes())

    def test_postprocessing_expiry_and_serialization_failure_preserve_old_output(self):
        output = self.directory / "qualification.json"
        output.write_text("old evidence", encoding="utf-8")
        protected = self.directory / "protected.bin"
        protected.write_bytes(b"protected")
        baseline = self.module.capture_identities([protected], float("inf"))

        # The deadline expires only after JSON was serialized, flushed, and the
        # final protected-input hash completed. Atomic replacement must not run.
        with (
            mock.patch.object(self.module.time, "monotonic",
                              side_effect=[0.0, 0.0, 0.0, 2.0]),
            self.assertRaises(self.module.HarnessError),
        ):
            self.module.publish_report(
                output, {"qualified": True}, 1.0, [protected], baseline)
        self.assertEqual(output.read_text(encoding="utf-8"), "old evidence")

        with self.assertRaises(ValueError):
            self.module.publish_report(
                output, {"value": float("nan")}, time.monotonic() + 2,
                [protected], baseline)
        self.assertEqual(output.read_text(encoding="utf-8"), "old evidence")
        self.assertEqual(list(self.directory.glob("*.tmp")), [])

    def test_final_identity_guard_detects_source_executable_and_metadata_changes(self):
        for name in ("source.stl", "benchmark.exe", "build-metadata.json"):
            with self.subTest(name=name):
                protected = self.directory / name
                protected.write_bytes(b"original")
                output = self.directory / f"{name}.report.json"
                output.write_bytes(b"old-report")
                baseline = self.module.capture_identities([protected], float("inf"))
                protected.write_bytes(b"modified")
                with self.assertRaises(self.module.HarnessError):
                    self.module.publish_report(
                        output, {"qualified": True}, time.monotonic() + 2,
                        [protected], baseline)
                self.assertEqual(output.read_bytes(), b"old-report")

    def test_per_workload_statistics_are_recomputed_from_samples(self):
        payload = self.payload()
        payload["workloads"][0]["runs"][1]["elapsed_ms"] = 9.0
        payload["workloads"][0]["runs"][2]["elapsed_ms"] = 1.0
        payload["workloads"][0]["runs"][3]["elapsed_ms"] = 4.0
        self.module.verify_payload(payload, self.expectations, 3, 1)
        performance = self.module.summarize_performance(payload)
        self.assertEqual(performance[0]["samples_ms"], [9.0, 1.0, 4.0])
        self.assertEqual(performance[0]["median_ms"], 4.0)
        self.assertEqual(performance[0]["p95_ms"], 9.0)

    def test_success_report_records_provenance_and_timeouts(self):
        payload = self.payload()
        executable = self.directory / "benchmark.exe"
        executable.write_bytes(b"benchmark")
        metadata = self.directory / "build.json"
        metadata.write_text('{"build_type":"Release","compiler":"fixture"}',
                            encoding="utf-8")
        output = self.directory / "qualification.json"
        root, manifest, expectations, _ = self.create_source_fixture()

        real_bindings = self.module.load_source_bindings(root, manifest, expectations)
        with (
            mock.patch.object(self.module, "ROOT", root),
            mock.patch.object(self.module, "MANIFEST", manifest),
            mock.patch.object(self.module, "EXPECT", self.expectations),
            mock.patch.object(self.module, "load_source_bindings",
                              return_value=real_bindings),
            mock.patch.object(self.module, "run_native", return_value=payload),
            mock.patch.object(self.module, "repository_metadata",
                              return_value={"revision": "abc123", "dirty": True}),
        ):
            result = self.module.main([
                "--executable", str(executable),
                "--build-metadata", str(metadata),
                "--output", str(output),
                "--timeout-seconds", "7",
                "--overall-timeout-seconds", "30",
            ])
        self.assertEqual(result, 0)
        report = json.loads(output.read_text(encoding="utf-8"))
        self.assertTrue(report["qualified"])
        self.assertEqual(report["metadata"]["repository"],
                         {"revision": "abc123", "dirty": True})
        self.assertEqual(report["metadata"]["parameters"]["timeout_seconds"], 7.0)
        self.assertEqual(report["metadata"]["parameters"]["overall_timeout_seconds"], 30.0)
        self.assertEqual(len(report["hashes"]["sources"]), 10)
        self.assertEqual(len(report["performance"]), 91)


if __name__ == "__main__":
    unittest.main()
