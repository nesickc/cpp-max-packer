import hashlib
import importlib.util
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).parents[1]
RUNNER_PATH = ROOT / "benchmarks" / "run_import.py"


def load_runner():
    specification = importlib.util.spec_from_file_location("run_import", RUNNER_PATH)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_cube_stl(path):
    vertices = [
        (0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
        (1.0, 1.0, 0.0), (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0), (1.0, 0.0, 1.0),
        (1.0, 1.0, 1.0), (0.0, 1.0, 1.0),
    ]
    faces = [
        (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
        (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2),
        (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5),
    ]
    data = bytearray(80)
    data.extend(struct.pack("<I", len(faces)))
    for face in faces:
        data.extend(struct.pack("<3f", 0, 0, 0))
        for vertex in face:
            data.extend(struct.pack("<3f", *vertices[vertex]))
        data.extend(struct.pack("<H", 0))
    path.write_bytes(data)
    return vertices, faces


def write_ply(path, vertices, faces):
    header = (
        "ply\nformat binary_little_endian 1.0\n"
        f"element vertex {len(vertices)}\n"
        "property double x\nproperty double y\nproperty double z\n"
        f"element face {len(faces)}\n"
        "property list uchar uint vertex_indices\nend_header\n"
    ).encode("ascii")
    body = bytearray(header)
    for vertex in vertices:
        body.extend(struct.pack("<3d", *vertex))
    for face in faces:
        body.extend(struct.pack("<B3I", 3, *face))
    path.write_bytes(body)


class ImportRunnerTests(unittest.TestCase):
    def setUp(self):
        self.runner = load_runner()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "cube.stl"
        vertices, faces = write_cube_stl(self.source)
        self.report_dir = self.root / "run"
        assets = self.report_dir / "assets"
        assets.mkdir(parents=True)
        source_copy = assets / f"{sha256(self.source)}.stl"
        source_copy.write_bytes(self.source.read_bytes())
        self.ply = assets / "cube.ply"
        write_ply(self.ply, vertices, faces)
        self.report = self.report_dir / "report.json"
        diagnostics = {
            "encoding": "binary", "source_byte_size": self.source.stat().st_size,
            "source_triangle_count": 12, "vertex_count": 8, "triangle_count": 12,
            "component_count": 1, "boundary_edges": 0, "nonmanifold_edges": 0,
            "nonmanifold_vertices": 0, "zero_area_faces": 0, "duplicate_faces": 0,
            "self_intersection_pairs": 0,
            "cleanup": {"exact_vertices_merged": 28, "duplicate_faces_removed": 0,
                        "zero_area_faces_removed": 0, "faces_reoriented": 0},
            "topology_check": "complete", "intersection_check": "complete",
            "containment_check": "complete", "candidate_pair_tests": 54,
            "predicate_work": 500, "issues_truncated": False,
            "shells": [{"id": 0, "parent_id": None, "depth": 0,
                        "triangle_count": 12, "input_orientation": "outward",
                        "final_orientation": "outward"}],
            "issues": [], "mesh_bounds_mm": {"min": [0, 0, 0], "max": [1, 1, 1]},
            "volume_mm3": 1.0,
        }
        self.value = {
            "schema_version": 1, "role": "container", "state": "accepted",
            "source": {"sha256": sha256(self.source),
                       "path": f"assets/{source_copy.name}", "units": "mm",
                       "unit_scale_mm": 1.0, "byte_size": self.source.stat().st_size},
            "frame": {"source_bounds": {"min": [0, 0, 0], "max": [1, 1, 1]},
                      "source_to_local": [[1, 0, 0, 0], [0, 1, 0, 0],
                                          [0, 0, 1, 0], [0, 0, 0, 1]]},
            "dimensions_mm": [1, 1, 1],
            "diagnostics": {"status": "valid", "messages": [], "import": diagnostics},
            "accepted_solid": {"sha256": sha256(self.ply), "path": "assets/cube.ply",
                               "format": "binary_little_endian_ply_f64_u32",
                               "vertex_count": 8, "triangle_count": 12},
            "repair_record": None,
        }
        self.report.write_text(json.dumps(self.value), encoding="utf-8")
        self.record = {
            "path": "rc/containers/cube.stl", "role": "container",
            "format": "binary", "bytes": self.source.stat().st_size,
            "sha256": sha256(self.source), "triangles": 12,
            "finite_coordinates": True,
            "bounds_source_units": [[0, 1], [0, 1], [0, 1]],
        }

    def tearDown(self):
        self.temporary.cleanup()

    def validate(self):
        return self.runner.validate_inspection(
            self.record, self.source, self.report, self.source.read_bytes())

    def test_validates_source_frame_binary_ply_and_box_volume(self):
        observed = self.validate()
        self.assertEqual(observed["status"], "valid")
        self.assertEqual(observed["artifact_kind"], "accepted_solid")
        self.assertEqual(observed["artifact_sha256"], sha256(self.ply))
        self.assertEqual(observed["counts"]["component_count"], 1)
        self.assertEqual(observed["volume_mm3"], 1.0)
        self.assertEqual(observed["mesh_bounds_mm"], {"min": [0, 0, 0], "max": [1, 1, 1]})
        self.assertEqual(len(observed["shells"]), 1)
        self.assertFalse(observed["issues_truncated"])

    def test_rejects_corrupted_hash_frame_ply_and_box_volume(self):
        mutations = [
            lambda value: value["accepted_solid"].update(sha256="0" * 64),
            lambda value: value["frame"]["source_to_local"][0].__setitem__(1, 1e-12),
            lambda value: value["diagnostics"]["import"].update(volume_mm3=2.0),
            lambda value: value.update(schema_version=999),
            lambda value: value["diagnostics"]["import"].update(component_count=True),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                value = json.loads(json.dumps(self.value))
                mutate(value)
                self.report.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaises(self.runner.HarnessError):
                    self.validate()
        self.report.write_text(json.dumps(self.value), encoding="utf-8")
        with self.ply.open("ab") as stream:
            stream.write(b"junk")
        with self.assertRaises(self.runner.HarnessError):
            self.validate()

    def test_rejects_duplicate_ply_face_that_hides_source_facet(self):
        vertices, faces = self.runner.read_binary_ply(self.ply)
        faces[1] = faces[0]
        write_ply(self.ply, vertices, faces)
        self.value["accepted_solid"]["sha256"] = sha256(self.ply)
        self.report.write_text(json.dumps(self.value), encoding="utf-8")
        with self.assertRaises(self.runner.HarnessError):
            self.validate()

    def test_expectations_require_complete_exact_observation(self):
        observed = self.validate()
        self.runner.compare_expectation(observed, dict(observed))
        changed = json.loads(json.dumps(observed))
        changed["checks"]["intersection"] = "not_run"
        with self.assertRaises(self.runner.HarnessError):
            self.runner.compare_expectation(observed, changed)
        incomplete = json.loads(json.dumps(observed))
        incomplete["status"] = "indeterminate"
        with self.assertRaises(self.runner.HarnessError):
            self.runner.require_qualifiable(incomplete)
        unsupported = json.loads(json.dumps(observed))
        unsupported["status"] = "invalid"
        unsupported["state"] = "inspected"
        unsupported["reasons"] = ["SELF_INTERSECTION"]
        unsupported["checks"]["intersection"] = "not_run"
        unsupported["counts"]["self_intersection_pairs"] = 0
        with self.assertRaises(self.runner.HarnessError):
            self.runner.require_qualifiable(unsupported)

    def test_item_expectations_bind_volume_and_shell_structure(self):
        self.record["role"] = "item"
        self.value["role"] = "object"
        for axis in range(3):
            self.value["frame"]["source_to_local"][axis][3] = -0.5
        vertices, faces = self.runner.read_binary_ply(self.ply)
        write_ply(
            self.ply,
            [tuple(coordinate - 0.5 for coordinate in vertex) for vertex in vertices],
            faces,
        )
        self.value["accepted_solid"]["sha256"] = sha256(self.ply)
        self.report.write_text(json.dumps(self.value), encoding="utf-8")
        expected = self.validate()

        mutations = [
            lambda value: value["diagnostics"]["import"].update(volume_mm3=2.0),
            lambda value: value["diagnostics"]["import"].update(shells=[]),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                changed = json.loads(json.dumps(self.value))
                mutate(changed)
                self.report.write_text(json.dumps(changed), encoding="utf-8")
                observed = self.validate()
                with self.assertRaises(self.runner.HarnessError):
                    self.runner.compare_expectation(observed, expected)

    def test_statistics_use_median_and_nearest_rank_p95(self):
        self.assertEqual(
            self.runner.statistics_for([9.0, 1.0, 4.0]),
            {"median_ms": 4.0, "p95_ms": 9.0},
        )

    def test_watchdog_terminates_and_reaps_child(self):
        deadline = time.monotonic() + 1
        with self.assertRaises(self.runner.HarnessError):
            self.runner.run_process(
                [sys.executable, "-c", "import time; time.sleep(5)"],
                timeout_seconds=0.02, deadline=deadline,
            )
        with mock.patch.object(self.runner.subprocess, "run") as launch:
            with self.assertRaises(self.runner.HarnessError):
                self.runner.run_process(
                    [sys.executable, "-c", "print('must not launch')"],
                    timeout_seconds=1, deadline=time.monotonic() - 1,
                )
            launch.assert_not_called()

    def test_final_artifact_validation_deadline_stops_before_next_launch(self):
        executable = self.root / "engine.exe"
        executable.write_bytes(b"engine")
        metadata = self.root / "build-metadata.json"
        metadata.write_text('{"build_type":"Release"}', encoding="utf-8")
        manifest = self.root / "manifest.json"
        manifest.write_text('{"schema_version":1,"records":[]}', encoding="utf-8")
        expectations_path = self.root / "expectations.json"
        expectations_path.write_text(
            '{"schema_version":1,"manifest_sha256":"unused","records":[]}',
            encoding="utf-8",
        )
        output = self.root / "qualification.json"
        records = []
        expectations = {}
        observed = self.validate()
        for index in range(10):
            relative = f"asset-{index}.stl"
            source = self.root / relative
            source.write_bytes(self.source.read_bytes())
            record = {"path": relative, "role": "item"}
            records.append(record)
            expectations[relative] = observed

        def completed_validation(*_args, **_kwargs):
            return observed, 1.0

        with (
            mock.patch.object(self.runner, "ROOT", self.root),
            mock.patch.object(self.runner, "DEFAULT_MANIFEST", manifest),
            mock.patch.object(self.runner, "load_manifest", return_value=records),
            mock.patch.object(
                self.runner, "load_expectations",
                return_value=(expectations, {"schema_version": 1}),
            ),
            mock.patch.object(
                self.runner, "invoke_inspection", side_effect=completed_validation,
            ) as launch,
            mock.patch.object(self.runner.time, "monotonic", side_effect=[100.0, 102.0]),
        ):
            result = self.runner.main([
                "--executable", str(executable),
                "--build-metadata", str(metadata),
                "--output", str(output),
                "--expectations", str(expectations_path),
                "--samples", "1",
                "--warmup", "0",
                "--overall-timeout-seconds", "1",
            ])
        self.assertEqual(result, 2)
        self.assertEqual(launch.call_count, 1)
        self.assertFalse(output.exists())

    def test_output_cannot_alias_any_input_lexically_or_by_identity(self):
        protected = [self.source, self.report]
        with self.assertRaises(self.runner.HarnessError):
            self.runner.ensure_distinct_output(self.source, protected)
        if hasattr(os, "link"):
            hardlink = self.root / "hardlink.json"
            os.link(self.source, hardlink)
            with self.assertRaises(self.runner.HarnessError):
                self.runner.ensure_distinct_output(hardlink, protected)

    def test_release_and_expectation_manifest_binding_are_strict(self):
        self.assertTrue(self.runner.release_metadata({"build_type": "Release"}))
        self.assertFalse(self.runner.release_metadata(
            {"build_type": "Debug", "unrelated": {"config": "Release"}}))
        document = {"schema_version": 1, "manifest_sha256": "a" * 64, "records": []}
        self.runner.require_expectation_manifest(document, "a" * 64)
        with self.assertRaises(self.runner.HarnessError):
            self.runner.require_expectation_manifest(document, "b" * 64)


if __name__ == "__main__":
    unittest.main()
