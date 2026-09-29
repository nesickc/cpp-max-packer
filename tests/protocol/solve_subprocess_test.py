"""AT-10/AT-14 black-box CLI artifact and rejection checks."""

import argparse
import copy
import hashlib
import json
import math
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest


ENGINE = None
CONTRACT_VALIDATOR = None
IDENTITY_CATALOG_SHA256 = "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240"
NONCARDINAL_CATALOG_SHA256 = "f74e79ad33ef648969371d789445424a68315f1f281a6620bf8b0d8a60fd8ff2"
NEAR_UNIT_FIXED_SHA256 = "6c42b5919d01d09e0007e22309b6bd04a2bc36dd78f9814d7dcc48ad519268d9"
NEAR_UNIT_CUSTOM_SHA256 = "15cb4952f292b8f531d9bf01aefda8ce2998eb8b9cf4a4675d35282861c0a999"
NEAR_UNIT_COMPONENT = 0.5000000000000001

SOURCE_FACES = [
    (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
    (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
    (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
]


def run_engine(*arguments):
    return subprocess.run(
        [str(ENGINE), *map(str, arguments)], capture_output=True, text=True,
        encoding="utf-8", timeout=25)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def float64_bits(value):
    return struct.pack("<d", value)


def float32_bits(value):
    return struct.pack("<f", value)


def cube_vertices(low=(0.0, 0.0, 0.0), extent=10.0):
    x, y, z = low
    return [
        (x, y, z), (x + extent, y, z), (x + extent, y + extent, z), (x, y + extent, z),
        (x, y, z + extent), (x + extent, y, z + extent),
        (x + extent, y + extent, z + extent), (x, y + extent, z + extent),
    ]


def write_cube_stl(path, low=(0.0, 0.0, 0.0), extent=10.0):
    vertices = cube_vertices(low, extent)
    data = bytearray(80) + struct.pack("<I", len(SOURCE_FACES))
    for face in SOURCE_FACES:
        data += struct.pack("<3f", 0.0, 0.0, 0.0)
        for vertex_index in face:
            data += struct.pack("<3f", *vertices[vertex_index])
        data += struct.pack("<H", 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return vertices


def parse_ply(path):
    data = path.read_bytes()
    marker = b"end_header\n"
    header_end = data.index(marker) + len(marker)
    header = data[:header_end].decode("ascii")
    lines = header.splitlines()
    vertex_count = int(next(line.split()[2] for line in lines if line.startswith("element vertex ")))
    face_count = int(next(line.split()[2] for line in lines if line.startswith("element face ")))
    expected_header = (
        "ply\nformat binary_little_endian 1.0\n"
        f"element vertex {vertex_count}\n"
        "property double x\nproperty double y\nproperty double z\n"
        f"element face {face_count}\n"
        "property list uchar uint vertex_indices\nend_header\n"
    )
    if header != expected_header:
        raise AssertionError(f"unexpected accepted PLY header: {header!r}")
    offset = header_end
    vertices = []
    for _ in range(vertex_count):
        vertices.append(struct.unpack_from("<3d", data, offset))
        offset += 24
    faces = []
    for _ in range(face_count):
        if data[offset] != 3:
            raise AssertionError("accepted PLY contains a non-triangle face")
        offset += 1
        face = struct.unpack_from("<3I", data, offset)
        offset += 12
        if any(index >= vertex_count for index in face):
            raise AssertionError("accepted PLY face index is outside the vertex table")
        faces.append(face)
    if offset != len(data):
        raise AssertionError("accepted PLY has trailing or truncated bytes")
    return data, vertices, faces


def parse_stl(path):
    data = path.read_bytes()
    if len(data) < 84:
        raise AssertionError("binary STL is shorter than its header")
    triangle_count = struct.unpack_from("<I", data, 80)[0]
    if len(data) != 84 + 50 * triangle_count:
        raise AssertionError("binary STL length disagrees with its triangle count")
    triangles = []
    for index in range(triangle_count):
        offset = 84 + 50 * index
        values = struct.unpack_from("<12f", data, offset)
        attribute = struct.unpack_from("<H", data, offset + 48)[0]
        points = tuple(
            (values[3 + 3 * corner], values[4 + 3 * corner], values[5 + 3 * corner])
            for corner in range(3))
        triangles.append((values[:3], points, attribute))
    return data, triangles


def quaternion_matrix(quaternion, translation):
    x, y, z, w = quaternion
    return [
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w),
         2.0 * (x * z + y * w), translation[0]],
        [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z),
         2.0 * (y * z - x * w), translation[1]],
        [2.0 * (x * z - y * w), 2.0 * (y * z + x * w),
         1.0 - 2.0 * (x * x + y * y), translation[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def rotate_and_translate(rotation, point, translation):
    return tuple(
        sum(rotation[row][column] * point[column] for column in range(3)) + translation[row]
        for row in range(3))


def expected_normal(points):
    edge_a = tuple(points[1][axis] - points[0][axis] for axis in range(3))
    edge_b = tuple(points[2][axis] - points[0][axis] for axis in range(3))
    cross = (
        edge_a[1] * edge_b[2] - edge_a[2] * edge_b[1],
        edge_a[2] * edge_b[0] - edge_a[0] * edge_b[2],
        edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0],
    )
    length = math.hypot(*cross)
    return tuple(component / length for component in cross) if length else (0.0, 0.0, 0.0)


def orientation_values(orientation):
    if orientation["mode"] == "fixed":
        return [orientation["quaternion_xyzw"]]
    return orientation["quaternions_xyzw"]


class SolveSubprocessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.source_path = self.root / "cube.stl"
        self.source_vertices = write_cube_stl(self.source_path)
        self.source_bytes = self.source_path.read_bytes()
        self.object_report_path = self.root / "object.json"
        completed = run_engine(
            "inspect", "--stl", self.source_path, "--units", "mm", "--role", "object",
            "--report", self.object_report_path)
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))

    def tearDown(self):
        self.temporary.cleanup()

    def settings(self, dimensions=(40.0, 40.0, 40.0), candidates=128, passes=1):
        report = json.loads(self.object_report_path.read_text(encoding="utf-8"))
        return {
            "settings_version": 1,
            "object_asset": {
                "source_sha256": report["source"]["sha256"],
                "accepted_solid_sha256": report["accepted_solid"]["sha256"],
            },
            "container": {"kind": "box", "dimensions_mm": list(dimensions)},
            "clearance_mm": {"pair": 0.0, "wall": 0.0},
            "orientation": {"mode": "fixed", "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0]},
            "search": {
                "preset": "subprocess", "seed": "0", "deterministic": True,
                "work_budget": {
                    "max_candidate_evaluations": candidates, "max_search_passes": passes,
                },
            },
            "resolution": {"mode": "manual", "pitch_mm": 10.0},
            "compute": {"backend": "cpu"},
            "resolved": {
                "pitch_mm": 10.0,
                "orientation_catalog_sha256": IDENTITY_CATALOG_SHA256,
                "orientation_catalog_version": 1,
                "backend": "cpu",
                "thread_count": 1,
            },
        }

    def solve(self, settings, output_root=None):
        output_root = output_root or self.root / "out"
        output_root.mkdir(parents=True, exist_ok=True)
        settings_path = output_root / "settings.json"
        result_path = output_root / "result.json"
        stl_path = output_root / "packed.stl"
        settings_path.write_text(json.dumps(settings), encoding="utf-8")
        completed = run_engine(
            "solve", "--settings", settings_path, "--object-report", self.object_report_path,
            "--result", result_path, "--stl", stl_path)
        if completed.returncode == 0:
            self.assert_result_contract(result_path)
        return completed, settings_path, result_path, stl_path

    def contract_check(self, document):
        return subprocess.run(
            [str(CONTRACT_VALIDATOR)], input=document, capture_output=True, timeout=10)

    def assert_result_contract(self, result_path):
        completed = self.contract_check(result_path.read_bytes())
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", errors="replace"))

    def assert_result_contract_rejected(self, document):
        completed = self.contract_check(document)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(
            "results contract validation failed",
            completed.stderr.decode("utf-8", errors="replace"))

    def assert_vector_close(self, actual, expected, places=6):
        self.assertEqual(len(actual), len(expected))
        for actual_value, expected_value in zip(actual, expected):
            self.assertAlmostEqual(actual_value, expected_value, places=places)

    def assert_matrix_close(self, actual, expected, places=12):
        self.assertEqual((len(actual), [len(row) for row in actual]), (4, [4, 4, 4, 4]))
        for actual_row, expected_row in zip(actual, expected):
            self.assert_vector_close(actual_row, expected_row, places)

    def assert_matrix_near(self, actual, expected, rel_tol, abs_tol):
        self.assertEqual((len(actual), [len(row) for row in actual]), (4, [4, 4, 4, 4]))
        for row, (actual_row, expected_row) in enumerate(zip(actual, expected)):
            for column, (actual_value, expected_value) in enumerate(zip(actual_row, expected_row)):
                self.assertTrue(
                    math.isclose(actual_value, expected_value, rel_tol=rel_tol, abs_tol=abs_tol),
                    (row, column, actual_value, expected_value, rel_tol, abs_tol))

    def portable_file(self, result_root, portable_path):
        self.assertIsInstance(portable_path, str)
        self.assertNotIn("\\", portable_path)
        relative = pathlib.PurePosixPath(portable_path)
        self.assertFalse(relative.is_absolute())
        self.assertTrue(relative.parts)
        self.assertTrue(all(part not in ("", ".", "..") and ":" not in part for part in relative.parts))
        root = result_root.resolve()
        target = (root / pathlib.Path(*relative.parts)).resolve()
        target.relative_to(root)
        self.assertTrue(target.is_file(), target)
        return target

    def assert_object_publication(self, result, result_root, expected_source_bytes,
                                  expected_local_vertices, expected_local_faces):
        asset = result["assets"]["object"]
        source = asset["source"]
        source_path = self.portable_file(result_root, source["path"])
        copied_source = source_path.read_bytes()
        self.assertEqual(copied_source, expected_source_bytes)
        self.assertEqual(source["byte_size"], len(expected_source_bytes))
        self.assertEqual(source["sha256"], sha256_bytes(expected_source_bytes))
        self.assertEqual(sha256_bytes(copied_source), source["sha256"])

        accepted = asset["accepted_solid"]
        accepted_path = self.portable_file(result_root, accepted["path"])
        accepted_bytes, vertices, faces = parse_ply(accepted_path)
        self.assertEqual(accepted["format"], "binary_little_endian_ply_f64_u32")
        self.assertEqual((accepted["vertex_count"], accepted["triangle_count"]),
                         (len(vertices), len(faces)))
        self.assertEqual(sha256_bytes(accepted_bytes), accepted["sha256"])
        def point_key(point):
            return tuple(round(value, 10) for value in point)

        self.assertEqual({point_key(point) for point in vertices},
                         {point_key(point) for point in expected_local_vertices})
        actual_triangles = [tuple(vertices[index] for index in face) for face in faces]
        self.assertEqual(
            {tuple(point_key(point) for point in triangle) for triangle in actual_triangles},
            {tuple(point_key(point) for point in triangle) for triangle in expected_local_faces})
        return asset, vertices, faces

    def artifact_and_companion(self, result, result_root):
        artifacts = [item for item in result.get("artifacts", []) if item["kind"] == "assembled_stl"]
        self.assertEqual(len(artifacts), 1)
        artifact = artifacts[0]
        stl_path = self.portable_file(result_root, artifact["path"])
        stl_bytes, triangles = parse_stl(stl_path)
        self.assertEqual(sha256_bytes(stl_bytes), artifact["sha256"])
        companion_path = pathlib.Path(str(stl_path) + ".json")
        self.assertTrue(companion_path.is_file())
        companion = json.loads(companion_path.read_text(encoding="utf-8"))
        return artifact, stl_bytes, triangles, companion

    def assert_companion(self, companion, result, artifact, triangles_per_copy):
        object_asset = result["assets"]["object"]
        self.assertEqual(companion["schema_version"], 1)
        self.assertEqual(companion["units"], "mm")
        self.assertEqual(companion["source_sha256"], object_asset["source"]["sha256"])
        self.assertEqual(companion["accepted_solid_sha256"],
                         object_asset["accepted_solid"]["sha256"])
        self.assertEqual(companion["assembly_sha256"], artifact["sha256"])
        self.assertEqual(companion["total_triangle_count"], triangles_per_copy * result["count"])
        self.assertEqual(len(companion["copies"]), result["count"])
        for index, copy_range in enumerate(companion["copies"]):
            self.assertEqual(copy_range, {
                "copy_id": result["placements"][index]["copy_id"],
                "first_triangle": triangles_per_copy * index,
                "triangle_count": triangles_per_copy,
            })

    def assert_triangle(self, actual, expected_points):
        normal, points, attribute = actual
        self.assertEqual(attribute, 0)
        self.assertTrue(all(math.isfinite(value) for value in normal))
        for actual_point, expected_point in zip(points, expected_points):
            self.assertEqual(
                [float32_bits(value) for value in actual_point],
                [float32_bits(value) for value in expected_point])
        self.assert_vector_close(normal, expected_normal(points), places=5)

    def assert_cube_artifacts(self, result_path, expected_source_bytes):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual((result["label"], result["count"], len(result["placements"])),
                         ("best_found", 64, 64))
        self.assertEqual(result["validation"]["status"], "valid")
        local_vertices = cube_vertices((-5.0, -5.0, -5.0), 10.0)
        local_faces = [tuple(local_vertices[index] for index in face) for face in SOURCE_FACES]
        asset, accepted_vertices, accepted_faces = self.assert_object_publication(
            result, result_path.parent, expected_source_bytes, local_vertices, local_faces)
        self.assert_matrix_close(asset["frame"]["source_to_local"], [
            [1.0, 0.0, 0.0, -5.0], [0.0, 1.0, 0.0, -5.0],
            [0.0, 0.0, 1.0, -5.0], [0.0, 0.0, 0.0, 1.0],
        ])
        self.assertEqual(asset["dimensions_mm"], [10.0, 10.0, 10.0])
        artifact, stl_bytes, triangles, companion = self.artifact_and_companion(
            result, result_path.parent)
        self.assertEqual(stl_bytes[:80], bytes(80))
        self.assertEqual(struct.unpack_from("<I", stl_bytes, 80)[0], 768)
        self.assertEqual(len(triangles), 768)
        self.assert_companion(companion, result, artifact, 12)

        expected_anchors = {
            (5.0 + 10.0 * x, 5.0 + 10.0 * y, 5.0 + 10.0 * z)
            for z in range(4) for y in range(4) for x in range(4)
        }
        self.assertEqual({tuple(item["translation_mm"]) for item in result["placements"]},
                         expected_anchors)
        copy_ids = [item["copy_id"] for item in result["placements"]]
        self.assertTrue(all(isinstance(copy_id, str) and copy_id for copy_id in copy_ids))
        self.assertEqual(len(copy_ids), len(set(copy_ids)))
        identity = [0.0, 0.0, 0.0, 1.0]

        def independent_local(local):
            matches = [
                point for point in local_vertices
                if all(abs(local[axis] - point[axis]) < 1e-12 for axis in range(3))
            ]
            self.assertEqual(len(matches), 1)
            return matches[0]

        for copy_index, placement in enumerate(result["placements"]):
            self.assertEqual(placement["quaternion_xyzw"], identity)
            translation = placement["translation_mm"]
            self.assert_matrix_close(placement["local_to_world"],
                                     quaternion_matrix(identity, translation))
            for face_index, face in enumerate(accepted_faces):
                expected_points = []
                for vertex in face:
                    local = independent_local(accepted_vertices[vertex])
                    expected_points.append(tuple(
                        local[axis] + translation[axis] for axis in range(3)))
                self.assert_triangle(triangles[12 * copy_index + face_index], expected_points)
        return result

    def input_snapshot(self, *extra_paths, object_report=None):
        report_path = object_report or self.object_report_path
        report = json.loads(report_path.read_text(encoding="utf-8"))
        paths = [
            self.source_path,
            report_path,
            report_path.parent / pathlib.Path(report["accepted_solid"]["path"]),
            *extra_paths,
        ]
        return {path.resolve(): path.read_bytes() for path in paths if path.exists() and path.is_file()}

    def assert_machine_error(self, completed, expected_codes=None):
        self.assertNotEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        records = [line for line in completed.stdout.splitlines() if line]
        self.assertEqual(len(records), 1, completed.stdout)
        reply = json.loads(records[0])
        self.assertEqual(reply["protocol_version"], 1)
        self.assertIsNone(reply["request_id"])
        self.assertFalse(reply["ok"])
        error = reply["error"]
        self.assertEqual(set(error), {"code", "message", "details", "recoverable"})
        self.assertIsInstance(error["code"], str)
        self.assertTrue(error["code"])
        self.assertIsInstance(error["message"], str)
        self.assertTrue(error["message"])
        self.assertIsInstance(error["details"], dict)
        self.assertIsInstance(error["recoverable"], bool)
        if expected_codes:
            self.assertIn(error["code"], expected_codes)
        return reply, error

    def assert_rejected(self, completed, result_path, stl_path, snapshot, expected_codes=None,
                        preexisting_outputs=()):
        _, error = self.assert_machine_error(completed, expected_codes)
        preexisting = {path.resolve() for path in preexisting_outputs}
        for output in (result_path, stl_path, pathlib.Path(str(stl_path) + ".json")):
            if output.resolve() not in preexisting:
                self.assertFalse(output.exists(), output)
        for path, before in snapshot.items():
            self.assertTrue(path.is_file(), path)
            self.assertEqual(path.read_bytes(), before, path)
        return error

    def write_case_settings(self, case_root, settings):
        case_root.mkdir(parents=True, exist_ok=True)
        settings_path = case_root / "settings.json"
        settings_path.write_text(json.dumps(settings), encoding="utf-8")
        return settings_path, case_root / "result.json", case_root / "packed.stl"

    def solve_arguments(self, settings_path, result_path, stl_path,
                        object_report=None, extra=()):
        return (
            "solve", "--settings", settings_path,
            "--object-report", object_report or self.object_report_path,
            "--result", result_path, "--stl", stl_path, *extra,
        )

    def test_cube_64(self):
        completed, _, result_path, _ = self.solve(self.settings())
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        result = self.assert_cube_artifacts(result_path, self.source_bytes)
        metrics = result["metrics"]
        self.assertEqual(
            (metrics["solid_volume_mm3"], metrics["container_volume_mm3"],
             metrics["utilization"]),
            (1000.0, 64000.0, 1.0))
        self.assertEqual(result["search"]["work_counts"], {
            "candidate_evaluations": 64, "search_passes": 1,
        })
        tampered = copy.deepcopy(result)
        del tampered["label"]
        self.assert_result_contract_rejected(
            json.dumps(tampered, separators=(",", ":")).encode("utf-8"))
        self.assertEqual(self.source_path.read_bytes(), self.source_bytes)

    def test_empty(self):
        completed, _, result_path, _ = self.solve(self.settings((5.0, 5.0, 5.0), 1, 1))
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        result = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual((result["label"], result["count"], result["placements"]),
                         ("best_found", 0, []))
        self.assertEqual(result["validation"]["status"], "valid")
        metrics = result["metrics"]
        self.assertEqual(
            (metrics["solid_volume_mm3"], metrics["container_volume_mm3"],
             metrics["utilization"]),
            (1000.0, 125.0, 0.0))
        local_vertices = cube_vertices((-5.0, -5.0, -5.0), 10.0)
        local_faces = [tuple(local_vertices[index] for index in face) for face in SOURCE_FACES]
        self.assert_object_publication(
            result, result_path.parent, self.source_bytes, local_vertices, local_faces)
        artifact, stl_bytes, triangles, companion = self.artifact_and_companion(
            result, result_path.parent)
        self.assertEqual(stl_bytes, bytes(84))
        self.assertEqual(triangles, [])
        self.assert_companion(companion, result, artifact, 12)
        self.assertEqual(self.source_path.read_bytes(), self.source_bytes)

    def test_unicode_relocation(self):
        output_root = self.root / "вивід" / "результат"
        completed, _, result_path, stl_path = self.solve(self.settings(), output_root)
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        reply = json.loads(completed.stdout)
        self.assertEqual(pathlib.Path(reply["result_path"]).resolve(), result_path.resolve())
        self.assertEqual(pathlib.Path(reply["stl_path"]).resolve(), stl_path.resolve())
        relocated_root = self.root / "переміщено"
        shutil.move(str(output_root), str(relocated_root))
        self.assertFalse(output_root.exists())
        relocated_result = relocated_root / result_path.name
        self.assert_result_contract(relocated_result)
        self.assert_cube_artifacts(relocated_result, self.source_bytes)
        self.assertEqual(self.source_path.read_bytes(), self.source_bytes)

    def test_transforms(self):
        self.source_path = self.root / "inch.stl"
        source_vertices = write_cube_stl(self.source_path, (2.0, 3.0, 4.0), 1.0)
        self.source_bytes = self.source_path.read_bytes()
        self.object_report_path = self.root / "inch.json"
        inspected = run_engine(
            "inspect", "--stl", self.source_path, "--units", "inch", "--role", "object",
            "--report", self.object_report_path)
        self.assertEqual(inspected.returncode, 0, (inspected.stdout, inspected.stderr))
        settings = self.settings((200.0, 200.0, 200.0), 2, 1)
        quaternion = [0.0, 0.0, 0.6, 0.8]
        settings["orientation"] = {"mode": "fixed", "quaternion_xyzw": quaternion}
        settings["resolved"]["orientation_catalog_sha256"] = NONCARDINAL_CATALOG_SHA256
        completed, _, result_path, _ = self.solve(settings)
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))

        result = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual(result["count"], 1)
        source_minimum = tuple(min(point[axis] for point in source_vertices) for axis in range(3))
        source_maximum = tuple(max(point[axis] for point in source_vertices) for axis in range(3))
        anchor = tuple(
            (25.4 * source_minimum[axis]) / 2.0 + (25.4 * source_maximum[axis]) / 2.0
            for axis in range(3))
        local_vertices = [
            tuple(25.4 * point[axis] - anchor[axis] for axis in range(3))
            for point in source_vertices
        ]
        local_faces = [tuple(local_vertices[index] for index in face) for face in SOURCE_FACES]
        asset, accepted_vertices, accepted_faces = self.assert_object_publication(
            result, result_path.parent, self.source_bytes, local_vertices, local_faces)
        self.assertEqual(asset["source"]["units"], "inch")
        self.assertEqual(asset["source"]["unit_scale_mm"], 25.4)
        self.assert_matrix_near(asset["frame"]["source_to_local"], [
            [25.4, 0.0, 0.0, -anchor[0]], [0.0, 25.4, 0.0, -anchor[1]],
            [0.0, 0.0, 25.4, -anchor[2]], [0.0, 0.0, 0.0, 1.0],
        ], rel_tol=1e-15, abs_tol=1e-15)

        placement = result["placements"][0]
        self.assertEqual([float64_bits(value) for value in placement["quaternion_xyzw"]],
                         [float64_bits(value) for value in quaternion])
        translation = placement["translation_mm"]
        expected_local_to_world = quaternion_matrix(quaternion, translation)
        self.assert_matrix_near(
            placement["local_to_world"], expected_local_to_world,
            rel_tol=1e-15, abs_tol=1e-15)
        analytic_rotation = [row[:3] for row in expected_local_to_world[:3]]
        rotation = [row[:3] for row in placement["local_to_world"][:3]]

        def source_for_local(local):
            matches = [
                point for point, expected_local in zip(source_vertices, local_vertices)
                if all(abs(local[axis] - expected_local[axis]) < 1e-10 for axis in range(3))
            ]
            self.assertEqual(len(matches), 1)
            return matches[0]

        artifact, stl_bytes, triangles, companion = self.artifact_and_companion(
            result, result_path.parent)
        self.assertEqual(stl_bytes[:80], bytes(80))
        self.assertEqual(len(triangles), 12)
        self.assert_companion(companion, result, artifact, 12)
        for face_index, face in enumerate(accepted_faces):
            expected_points = []
            for vertex_index in face:
                source_point = source_for_local(accepted_vertices[vertex_index])
                local_point = tuple(
                    25.4 * source_point[axis] - anchor[axis] for axis in range(3))
                analytic_point = rotate_and_translate(
                    analytic_rotation, local_point, translation)
                published_point = rotate_and_translate(rotation, local_point, translation)
                # Equivalent robust quaternion evaluation can differ by one float64 ULP;
                # cancellation near zero exposes that difference before float32 export.
                for actual_value, expected_value in zip(published_point, analytic_point):
                    self.assertTrue(math.isclose(
                        actual_value, expected_value, rel_tol=2e-15, abs_tol=5e-14))
                expected_points.append(published_point)
            self.assert_triangle(triangles[face_index], expected_points)
        self.assertEqual(self.source_path.read_bytes(), self.source_bytes)

    def test_resolved_catalog(self):
        q1 = [NEAR_UNIT_COMPONENT] * 4
        cases = [
            (
                "fixed", [q1],
                '{"version":1,"quaternions_xyzw":[[0.5000000000000001,0.5000000000000001,'
                '0.5000000000000001,0.5000000000000001]]}',
                NEAR_UNIT_FIXED_SHA256,
            ),
            (
                "custom", [q1, [0.0, 0.0, 0.0, 1.0]],
                '{"version":1,"quaternions_xyzw":[[0.5000000000000001,0.5000000000000001,'
                '0.5000000000000001,0.5000000000000001],[0.0,0.0,0.0,1.0]]}',
                NEAR_UNIT_CUSTOM_SHA256,
            ),
        ]
        for mode, quaternions, canonical_text, golden_hash in cases:
            with self.subTest(mode=mode):
                self.assertEqual(sha256_bytes(canonical_text.encode("utf-8")), golden_hash)
                settings = self.settings((40.0, 40.0, 40.0), 2, 1)
                if mode == "fixed":
                    settings["orientation"] = {"mode": mode, "quaternion_xyzw": quaternions[0]}
                else:
                    settings["orientation"] = {"mode": mode, "quaternions_xyzw": quaternions}
                settings["resolved"]["orientation_catalog_sha256"] = golden_hash
                completed, _, result_path, _ = self.solve(settings, self.root / mode)
                self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
                result = json.loads(result_path.read_text(encoding="utf-8"))
                expected_bits = [[float64_bits(value) for value in item] for item in quaternions]
                constraint_bits = [
                    [float64_bits(value) for value in item]
                    for item in orientation_values(result["constraints"]["orientation"])
                ]
                self.assertEqual(constraint_bits, expected_bits)
                self.assertEqual(result["constraints"]["orientation_catalog_sha256"], golden_hash)
                resolved_settings = [result["search"]["resolved_settings"]]
                resolved_settings.extend(
                    segment["resolved_settings"] for segment in result["search"]["run_segments"])
                for resolved in resolved_settings:
                    actual_bits = [
                        [float64_bits(value) for value in item]
                        for item in orientation_values(resolved["orientation"])
                    ]
                    self.assertEqual(actual_bits, expected_bits)
                    self.assertEqual(resolved["resolved"]["orientation_catalog_sha256"], golden_hash)
                allowed_pose_bits = {tuple(item) for item in expected_bits}
                pose_bits = [
                    tuple(float64_bits(value) for value in placement["quaternion_xyzw"])
                    for placement in result["placements"]
                ]
                self.assertTrue(pose_bits)
                self.assertEqual(pose_bits[0], tuple(expected_bits[0]))
                self.assertTrue(all(item in allowed_pose_bits for item in pose_bits))

        stale = self.settings(candidates=1, passes=1)
        stale["resolved"]["orientation_catalog_sha256"] = "0" * 64
        completed, settings_path, result_path, stl_path = self.solve(stale, self.root / "stale")
        snapshot = self.input_snapshot(settings_path)
        self.assert_rejected(
            completed, result_path, stl_path, snapshot, {"CATALOG_MISMATCH"})
        self.assertFalse((result_path.parent / "assets").exists())

    def test_rejections(self):
        rejection_root = self.root / "rejections"
        rejection_root.mkdir()
        base_settings = self.settings(candidates=1, passes=1)

        def execute(name, settings=None, arguments=None, object_report=None,
                    expected_codes=None, prepare=None, preexisting_outputs=()):
            case_root = rejection_root / name
            settings_path, result_path, stl_path = self.write_case_settings(
                case_root, settings if settings is not None else base_settings)
            if prepare:
                prepare(settings_path)
            if arguments is None:
                arguments = self.solve_arguments(
                    settings_path, result_path, stl_path, object_report)
            snapshot = self.input_snapshot(
                settings_path, *(path for path in preexisting_outputs if path.is_file()),
                object_report=object_report)
            completed = run_engine(*arguments)
            self.assert_rejected(
                completed, result_path, stl_path, snapshot, expected_codes, preexisting_outputs)
            return case_root, settings_path, result_path, stl_path

        missing_root = rejection_root / "missing-option"
        missing_settings, _, _ = self.write_case_settings(missing_root, base_settings)
        execute(
            "missing-option",
            arguments=("solve", "--settings", missing_settings,
                       "--object-report", self.object_report_path),
            expected_codes={"INVALID_REQUEST"},
        )
        duplicate_root = rejection_root / "duplicate-option"
        execute(
            "duplicate-option",
            arguments=self.solve_arguments(
                duplicate_root / "settings.json", duplicate_root / "result.json",
                duplicate_root / "packed.stl",
                extra=("--settings", duplicate_root / "settings.json")),
            expected_codes={"INVALID_REQUEST"},
        )
        unknown_root = rejection_root / "unknown-option"
        execute(
            "unknown-option",
            arguments=self.solve_arguments(
                unknown_root / "settings.json", unknown_root / "result.json",
                unknown_root / "packed.stl", extra=("--unknown", "value")),
            expected_codes={"INVALID_REQUEST"},
        )
        execute("missing-value", arguments=("solve", "--settings"),
                expected_codes={"INVALID_REQUEST"})

        unreadable_root = rejection_root / "unreadable-settings"
        unreadable_root.mkdir()
        unreadable_result = unreadable_root / "result.json"
        unreadable_stl = unreadable_root / "packed.stl"
        unreadable_completed = run_engine(*self.solve_arguments(
            unreadable_root / "absent.json", unreadable_result, unreadable_stl))
        self.assert_rejected(
            unreadable_completed, unreadable_result, unreadable_stl,
            self.input_snapshot(), {"SETTINGS_LOAD"})

        execute(
            "malformed-settings", expected_codes={"INVALID_SETTINGS"},
            prepare=lambda path: path.write_text("{bad", encoding="utf-8"))

        stale = copy.deepcopy(base_settings)
        stale["resolved"]["orientation_catalog_sha256"] = "0" * 64
        stale_root, _, _, _ = execute(
            "stale-catalog", stale, expected_codes={"CATALOG_MISMATCH"})
        self.assertFalse((stale_root / "assets").exists())

        bad_report = self.root / "mismatched-object.json"
        bad_report_value = json.loads(self.object_report_path.read_text(encoding="utf-8"))
        bad_report_value["source"]["sha256"] = "f" * 64
        bad_report.write_text(json.dumps(bad_report_value), encoding="utf-8")
        execute("mismatched-report", object_report=bad_report,
                expected_codes={"ASSET_MISMATCH"})

        unsupported_orientation = copy.deepcopy(base_settings)
        unsupported_orientation["orientation"] = {"mode": "free", "catalog_size": 8}
        execute("unsupported-orientation", unsupported_orientation,
                expected_codes={"UNSUPPORTED_SETTINGS"})

        unavailable_backend = copy.deepcopy(base_settings)
        unavailable_backend["search"] = {
            "preset": "subprocess", "seed": "0", "deterministic": False,
            "budget_seconds": 1.0,
        }
        unavailable_backend["compute"]["backend"] = "vulkan"
        unavailable_backend["resolved"]["backend"] = "vulkan"
        execute("unsupported-backend", unavailable_backend,
                expected_codes={"UNSUPPORTED_SETTINGS"})

        box_root = rejection_root / "box-container-report"
        execute(
            "box-container-report",
            arguments=self.solve_arguments(
                box_root / "settings.json", box_root / "result.json", box_root / "packed.stl",
                extra=("--container-report", self.object_report_path)),
            expected_codes={"ASSET_MISMATCH"},
        )

        stl_container = copy.deepcopy(base_settings)
        object_report = json.loads(self.object_report_path.read_text(encoding="utf-8"))
        stl_container["container"] = {
            "kind": "stl_volume",
            "asset": {
                "source_sha256": object_report["source"]["sha256"],
                "accepted_solid_sha256": object_report["accepted_solid"]["sha256"],
            },
        }
        execute("missing-container-report", stl_container,
                expected_codes={"ASSET_MISMATCH"})
        mismatched_container = copy.deepcopy(stl_container)
        mismatched_container["container"]["asset"]["source_sha256"] = "e" * 64
        mismatch_root = rejection_root / "mismatched-container-report"
        execute(
            "mismatched-container-report", mismatched_container,
            arguments=self.solve_arguments(
                mismatch_root / "settings.json", mismatch_root / "result.json",
                mismatch_root / "packed.stl",
                extra=("--container-report", self.object_report_path)),
            expected_codes={"ASSET_MISMATCH"},
        )

        alias_root = rejection_root / "ordinary-alias"
        alias_settings, _, alias_stl = self.write_case_settings(alias_root, base_settings)
        alias_snapshot = self.input_snapshot(alias_settings)
        alias_completed = run_engine(
            "solve", "--settings", alias_settings, "--object-report", self.object_report_path,
            "--result", alias_settings, "--stl", alias_stl)
        self.assert_rejected(
            alias_completed, alias_settings, alias_stl, alias_snapshot,
            {"INVALID_REQUEST"}, (alias_settings,))

        hardlink_root = rejection_root / "hardlink-alias"
        hardlink_settings, _, hardlink_stl = self.write_case_settings(hardlink_root, base_settings)
        hardlink_result = hardlink_root / "result-hardlink.json"
        os.link(hardlink_settings, hardlink_result)
        hardlink_snapshot = self.input_snapshot(hardlink_settings, hardlink_result)
        hardlink_completed = run_engine(
            "solve", "--settings", hardlink_settings, "--object-report", self.object_report_path,
            "--result", hardlink_result, "--stl", hardlink_stl)
        self.assert_rejected(
            hardlink_completed, hardlink_result, hardlink_stl, hardlink_snapshot,
            {"INVALID_REQUEST"}, (hardlink_result,))

        if os.name == "nt":
            junction_root = rejection_root / "junction-alias"
            real_root = junction_root / "real"
            real_settings, _, junction_stl = self.write_case_settings(real_root, base_settings)
            junction = junction_root / "alternate"
            created = subprocess.run(
                ["cmd.exe", "/d", "/c", "mklink", "/J", str(junction), str(real_root)],
                capture_output=True, text=True,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.assertEqual(created.returncode, 0, (created.stdout, created.stderr))
            junction_result = junction / real_settings.name
            junction_snapshot = self.input_snapshot(real_settings, junction_result)
            junction_completed = run_engine(
                "solve", "--settings", real_settings, "--object-report", self.object_report_path,
                "--result", junction_result, "--stl", junction_stl)
            self.assert_rejected(
                junction_completed, junction_result, junction_stl, junction_snapshot,
                {"INVALID_REQUEST"}, (junction_result,))

        existing_root = rejection_root / "existing-stl"
        existing_settings, existing_result, existing_stl = self.write_case_settings(
            existing_root, base_settings)
        existing_stl.write_bytes(b"existing-stl-sentinel")
        existing_snapshot = self.input_snapshot(existing_settings, existing_stl)
        existing_completed = run_engine(*self.solve_arguments(
            existing_settings, existing_result, existing_stl))
        existing_reply, _ = self.assert_machine_error(
            existing_completed, {"EXPORT_WRITE_FAILED"})
        self.assertEqual(existing_stl.read_bytes(), b"existing-stl-sentinel")
        self.assertFalse(pathlib.Path(str(existing_stl) + ".json").exists())
        self.assert_result_contract(existing_result)
        retained = json.loads(existing_result.read_text(encoding="utf-8"))
        self.assertEqual((retained["label"], retained["count"], retained["validation"]["status"]),
                         ("best_found", 1, "valid"))
        self.assertEqual(pathlib.Path(existing_reply["error"]["details"]["result_path"]).resolve(),
                         existing_result.resolve())
        for path, before in existing_snapshot.items():
            self.assertEqual(path.read_bytes(), before, path)

        report_alias_root = rejection_root / "report-alias"
        report_alias_settings, _, report_alias_stl = self.write_case_settings(
            report_alias_root, base_settings)
        report_alias_snapshot = self.input_snapshot(report_alias_settings)
        report_alias_completed = run_engine(
            "solve", "--settings", report_alias_settings,
            "--object-report", self.object_report_path,
            "--result", self.object_report_path, "--stl", report_alias_stl)
        self.assert_rejected(
            report_alias_completed, self.object_report_path, report_alias_stl,
            report_alias_snapshot, {"EXPORT_PATH_INVALID"}, (self.object_report_path,))

        same_output_root = rejection_root / "same-output"
        same_settings, same_result, _ = self.write_case_settings(same_output_root, base_settings)
        same_snapshot = self.input_snapshot(same_settings)
        same_completed = run_engine(
            "solve", "--settings", same_settings, "--object-report", self.object_report_path,
            "--result", same_result, "--stl", same_result)
        self.assert_rejected(
            same_completed, same_result, same_result, same_snapshot,
            {"EXPORT_PATH_INVALID"})

        # Keep the known product defect last so every independent rejection above still runs.
        inconsistent_pitch = copy.deepcopy(base_settings)
        inconsistent_pitch["resolution"]["pitch_mm"] = 9.0
        execute("inconsistent-pitch", inconsistent_pitch,
                expected_codes={"INVALID_SETTINGS", "UNSUPPORTED_SETTINGS"})
        self.assertEqual(self.source_path.read_bytes(), self.source_bytes)


CASES = {
    "solve_cube_64": "test_cube_64",
    "solve_empty": "test_empty",
    "solve_unicode_relocation": "test_unicode_relocation",
    "solve_transforms": "test_transforms",
    "solve_resolved_catalog": "test_resolved_catalog",
    "solve_rejections": "test_rejections",
}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("--contract-validator", required=True)
    parser.add_argument("--case", choices=CASES)
    arguments = parser.parse_args()
    ENGINE = pathlib.Path(arguments.engine)
    CONTRACT_VALIDATOR = pathlib.Path(arguments.contract_validator)
    if arguments.case:
        suite = unittest.TestSuite([SolveSubprocessTests(CASES[arguments.case])])
    else:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SolveSubprocessTests)
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
