"""Independent qualification checks for the native representation payload."""

import copy
import importlib.util
import json
import math
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
EXPECTATIONS = ROOT / "tests" / "fixtures" / "import-expectations.json"


def load_checks():
    spec = importlib.util.spec_from_file_location(
        "representation_checks", ROOT / "benchmarks" / "representation_checks.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _expectations():
    records = json.loads(EXPECTATIONS.read_text(encoding="utf-8"))["records"]
    return {record["path"]: record["expected"] for record in records}


def _fnv(cells):
    value = 1469598103934665603
    for cell in cells:
        value = ((value ^ cell) * 1099511628211) & ((1 << 64) - 1)
    return value


def _field(first, shape, purpose, occupied):
    count = math.prod(shape)
    cells = bytearray(count)
    for x, y, z in occupied:
        local = (x - first[0], y - first[1], z - first[2])
        if all(0 <= local[axis] < shape[axis] for axis in range(3)):
            cells[local[0] + shape[0] * (local[1] + shape[1] * local[2])] = 1
    packed = bytearray((count + 7) // 8)
    for index, cell in enumerate(cells):
        if cell:
            packed[index // 8] |= 1 << (index % 8)
    return {
        "window": {"origin_mm": [0.0, 0.0, 0.0], "pitch_mm": 1.0,
                   "first": list(first), "shape": list(shape)},
        "purpose": purpose,
        "encoding": "bitpacked_x_fast_lsb",
        "bits_hex": packed.hex(),
        "fingerprint": _fnv(cells),
        "stats": {"working_bytes_peak": count + 100, "kernel_work": count * 2,
                  "cell_visits": count, "occupied_cells": sum(cells),
                  "uncertain_cells": 0},
    }


def _set_canonical_bit(field, cell, value):
    window = field["window"]
    local = [cell[axis] - window["first"][axis] for axis in range(3)]
    index = local[0] + window["shape"][0] * (
        local[1] + window["shape"][1] * local[2])
    packed = bytearray.fromhex(field["bits_hex"])
    old = (packed[index // 8] >> (index % 8)) & 1
    if value:
        packed[index // 8] |= 1 << (index % 8)
    else:
        packed[index // 8] &= ~(1 << (index % 8))
    cells = bytearray((packed[item // 8] >> (item % 8)) & 1
                      for item in range(math.prod(window["shape"])))
    field["bits_hex"] = packed.hex()
    field["fingerprint"] = _fnv(cells)
    field["stats"]["occupied_cells"] += value - old


def _set_workload_field_bit(workload, field_name, cell, value):
    canonical = workload["canonical_fields"][field_name]
    _set_canonical_bit(canonical, cell, value)
    for run in workload["warmups"] + workload["samples"]:
        for side in ("field_a", "field_b"):
            report = run[side]["fields"][field_name]
            report["fingerprint"] = canonical["fingerprint"]
            report["stats"]["occupied_cells"] = canonical["stats"]["occupied_cells"]
        run["run_fingerprints"][field_name] = canonical["fingerprint"]


def _cube_cells(low, high):
    return {(x, y, z) for z in range(low, high + 1)
            for y in range(low, high + 1) for x in range(low, high + 1)}


def _validation(translation):
    report = {
        "validity": 0, "code": "VALID", "message": "complete",
        "epsilon_mm": 1e-6, "kernel_revision": "homogeneous-rational-interval-v1",
        "aabb_pair_tests": 0, "kernel_work": 100, "working_bytes_peak": 1000,
        "affected_ids_truncated": False, "affected_copy_ids": [],
        "checks": [{"check": index, "state": 1, "method": "checked"}
                   for index in range(6)],
    }
    pose = {"id": "known-valid", "translation_mm": list(translation),
            "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]}
    return {"report": report, "has_snapshot": True,
            "snapshot": {"context_address": 1234, "poses": [pose],
                         "report": copy.deepcopy(report)}}


def _solid(path, expected, *, large=False):
    accepted_bounds = ({"min": [-150.0] * 3, "max": [150.0] * 3}
                       if large else expected["mesh_bounds_mm"])
    source_bounds = ({"min": [0.0] * 3, "max": [300.0] * 3}
                     if large else expected["mesh_bounds_mm"])
    dimensions = [source_bounds["max"][i] - source_bounds["min"][i]
                  for i in range(3)]
    return {
        "path": path, "source_bytes": 54_000_084 if large else 100,
        "source_fingerprint": 123456,
        "frame": {"source_bounds": source_bounds, "unit_scale_mm": 1.0,
                  "anchor_mm": [150.0] * 3 if large else [0.0] * 3,
                  "dimensions_mm": dimensions},
        "accepted": {"vertices": 540_002 if large else expected["counts"]["vertex_count"],
                     "triangles": 1_080_000 if large else expected["counts"]["triangle_count"],
                     "bounds_mm": accepted_bounds,
                     "volume_mm3": 27_000_000.0 if large else expected["volume_mm3"],
                     "vertex_fingerprint": 77, "triangle_fingerprint": 88},
        "import_diagnostics": {
            "validity": 0,
            "source_triangles": 1_080_000 if large else expected["counts"]["source_triangle_count"],
            "candidate_pair_tests": 9_179_952 if large else expected["work_counts"]["candidate_pair_tests"],
            "predicate_work": 13_737_868_762 if large else expected["work_counts"]["predicate_work"],
            "boundary_edges": 0, "nonmanifold_edges": 0,
            "self_intersection_pairs": 0, "topology_check": 1,
            "intersection_check": 1,
        },
    }


def _run(name, expected, pitch, *, large=False, sample_index=0):
    container_expected = _expectations()["rc/containers/5_kg_np.stl"]
    dimensions = container_expected["mesh_bounds_mm"]["max"]
    environment_shape = ([20, 20, 20] if large else
                         [math.ceil(dimensions[i] / pitch) for i in range(3)])
    bounds = ({"min": [-150.0] * 3, "max": [150.0] * 3}
              if large else expected["mesh_bounds_mm"])
    object_first = [math.floor(bounds["min"][i] / pitch) - 1 for i in range(3)]
    object_last = [math.floor(bounds["max"][i] / pitch) + 1 for i in range(3)]
    object_shape = [object_last[i] - object_first[i] + 1 for i in range(3)]
    pose = ([200.0] * 3 if large else
            [(dimensions[i] - bounds["min"][i] - bounds["max"][i]) / 2
             for i in range(3)])
    if large:
        object_cells = _cube_cells(-8, 7)
        placed_cells = _cube_cells(1, 18)
        container_cells = {(x, y, z) for z in range(20) for y in range(20) for x in range(20)
                           if x in (0, 19) or y in (0, 19) or z in (0, 19)}
    else:
        object_cells = {(x, y, z) for z in range(object_first[2] + 1, object_last[2])
                        for y in range(object_first[1] + 1, object_last[1])
                        for x in range(object_first[0] + 1, object_last[0])}
        placed_cells = set()
        for z in range(environment_shape[2]):
            for y in range(environment_shape[1]):
                for x in range(environment_shape[0]):
                    cell_low = [x * pitch, y * pitch, z * pitch]
                    cell_high = [(x + 1) * pitch, (y + 1) * pitch, (z + 1) * pitch]
                    if all(cell_high[i] >= bounds["min"][i] + pose[i] - 1 and
                           cell_low[i] <= bounds["max"][i] + pose[i] + 1 for i in range(3)):
                        placed_cells.add((x, y, z))
        container_cells = {(x, y, z) for z in range(environment_shape[2])
                           for y in range(environment_shape[1]) for x in range(environment_shape[0])
                           if any(index * pitch < 1 or (index + 1) * pitch > dimensions[axis] - 1
                                  for axis, index in enumerate((x, y, z)))}
    fields = {
        "object": _field(object_first, object_shape, "object_kernel", object_cells),
        "placed": _field([0, 0, 0], environment_shape, "placed_pair_blocker", placed_cells),
        "container": _field([0, 0, 0], environment_shape, "container_blocker", container_cells),
    }
    for field in fields.values():
        field["window"]["pitch_mm"] = float(pitch)
    local_point = list(bounds["min"])
    object_surface = [math.floor(value / pitch) for value in local_point]
    physical_point = [local_point[i] + pose[i] for i in range(3)]
    placed_surface = [math.floor(value / pitch) for value in physical_point]
    exterior = [math.floor(bounds["min"][i] / pitch) - 1 for i in range(3)]
    witnesses = [
        {"kind": "object_surface", "index": 0, "local_point_mm": local_point,
         "cell": object_surface, "expected_bit": 1},
        {"kind": "object_exterior_strict_whole_cell", "cell": exterior,
         "enclosure_bounds_mm": copy.deepcopy(bounds), "expected_bit": 0},
        {"kind": "placed_surface", "index": 0, "local_point_mm": local_point,
         "physical_point_mm": physical_point, "cell": placed_surface, "expected_bit": 1},
        {"kind": "placed_exterior", "cell": [0, 0, 0], "expected_bit": 0},
        {"kind": "container_wall", "cell": [0, 0, 0], "expected_bit": 1},
        {"kind": "container_interior", "cell": [x // 2 for x in environment_shape],
         "expected_bit": 0},
    ]
    if large:
        witnesses += [{"kind": "large_placed_exterior", "cell": [0, 0, 0],
                       "expected_bit": 0},
                      {"kind": "large_exact_counts", "object": 4096,
                       "placed": 5832, "container": 2168}]
    def without_bits(field_map):
        value = copy.deepcopy(field_map)
        for field in value.values():
            field.pop("bits_hex")
        return value
    timing = 1.0 + sample_index
    field_block = {"timings": {"prepare_ms": timing, "object_field_ms": timing + .1,
                                "placed_field_ms": timing + .2,
                                "container_field_ms": timing + .3},
                   "fields": copy.deepcopy(fields), "witnesses": copy.deepcopy(witnesses)}
    validation = _validation(pose)
    target_b, error_b = ((1000, .01) if large else (2000, .5))
    source_triangles = 1_080_000 if large else expected["counts"]["triangle_count"]
    def lod(actual, target, error):
        return {"source_triangles": source_triangles, "actual_triangles": actual,
                "requested_error_mm": error, "approximate_error_mm": error / 2,
                "coordinate_conversion_error_mm": 0.0,
                "target_reached": actual <= target, "vertex_fingerprint": actual + 1,
                "triangle_fingerprint": actual + 2, "tracked_peak_bytes": 10_000}
    cap = 2 * 1024**3 if large else 512 * 1024**2
    run = {
        "import_setup_ms": timing,
        "object": _solid("cube_subdivided_n300.stl" if large else
                         (ROOT / name).as_posix(), expected, large=large),
        "lod_a_ms": timing + .4, "lod_a": lod(min(source_triangles, 20_000), 20_000, .1),
        "field_a": field_block, "validation_a_ms": timing + .5,
        "validation_before": validation,
        "lod_b_ms": timing + .6, "lod_b": lod(min(source_triangles, target_b), target_b, error_b),
        "field_b": {"timings": copy.deepcopy(field_block["timings"]),
                    "fields": without_bits(fields), "witnesses": copy.deepcopy(witnesses)},
        "validation_b_ms": timing + .7, "validation_after": copy.deepcopy(validation),
        "settings": {"pitch_mm": pitch,
                     "window": {"origin_mm": [0.0, 0.0, 0.0], "first": [0, 0, 0],
                                "shape": environment_shape},
                     "pair_clearance_mm": 1.0, "wall_clearance_mm": 1.0,
                     "requested_lod_a": {"target_triangles": 20_000, "max_error_mm": .1},
                     "requested_lod_b": {"target_triangles": target_b, "max_error_mm": error_b},
                     "large_import_overrides": ({"max_predicate_work": 40_000_000_000,
                                                  "max_candidate_pairs": 50_000_000} if large else {}),
                     "large_representation_overrides": ({"max_kernel_work": 16_000_000_000,
                                                          "max_working_bytes": 2 * 1024**3} if large else {}),
                     "representation_cap_bytes": cap, "retained_reserve_bytes": 100_000},
        "run_fingerprints": {key: value["fingerprint"] for key, value in fields.items()},
    }
    if not large:
        for field in run["field_a"]["fields"].values():
            field.pop("bits_hex")
    if not large:
        run["container"] = _solid((ROOT / "rc/containers/5_kg_np.stl").as_posix(),
                                  container_expected)
    else:
        invalid_report = copy.deepcopy(validation["report"])
        invalid_report.update({"validity": 1, "code": "VALIDATION_CONTAINER",
                               "affected_copy_ids": ["through-wall"]})
        run["through_wall_validation"] = {"report": invalid_report, "has_snapshot": False}
    return run, fields


def valid_payload(samples=3, warmup=1):
    expectations = _expectations()
    workloads = []
    for name, pitch in (("rc/items/pryanik_1.STL", 4),
                        ("rc/items/pryanik_2.STL", 4),
                        ("rc/items/ulamok_2kg_simplified.stl", 10)):
        runs = [_run(name, expectations[name], pitch, sample_index=i)[0]
                for i in range(warmup + samples)]
        canonical = _run(name, expectations[name], pitch)[1]
        workloads.append({"name": name, "warmups": runs[:warmup],
                          "samples": runs[warmup:], "canonical_fields": canonical})
    large_expected = {"counts": {"source_triangle_count": 1_080_000,
                                  "vertex_count": 540_002, "triangle_count": 1_080_000},
                      "work_counts": {},
                      "mesh_bounds_mm": {"min": [-150.0] * 3, "max": [150.0] * 3},
                      "volume_mm3": 27_000_000.0}
    large_run, _ = _run("subdivided_cube_n300", large_expected, 20, large=True)
    workloads.append({"name": "subdivided_cube_n300",
                      "expected": {"triangles": 1_080_000, "vertices": 540_002,
                                   "volume_mm3": 27_000_000.0, "source_bytes": 54_000_084},
                      "run": large_run})
    return {"schema_version": 1, "benchmark_kind": "native_representation_qualification",
            "build": {"compiler": "msvc", "compiler_version": 194400000,
                      "build_type": "Release"},
            "parameters": {"samples": samples, "warmup": warmup, "large_only": False,
                           "large_representation_cap_bytes": 2 * 1024**3},
            "process_peak_working_set_bytes_before": 10_000,
            "workloads": workloads, "process_peak_working_set_bytes_after": 20_000}


class RepresentationQualificationChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checks = load_checks()

    def test_strict_json_rejects_duplicate_nonfinite_and_trailing_documents(self):
        for text in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":1e999}',
                     '{"a":1}\n{"b":2}'):
            with self.subTest(text=text), self.assertRaises(self.checks.PayloadError):
                self.checks.strict_json(text)
        self.assertEqual(self.checks.strict_json('{"a":1}'), {"a": 1})

    def test_valid_payload_returns_distribution_and_single_observation(self):
        payload = valid_payload()
        performance = self.checks.verify_payload(payload, EXPECTATIONS, 3, 1)
        phase = performance["small_workloads"][0]["phases"]["import_setup_ms"]
        self.assertEqual(phase["raw_ms"], [2.0, 3.0, 4.0])
        self.assertEqual(phase["median_ms"], 3.0)
        self.assertEqual(phase["p95_ms"], 4.0)
        self.assertIn("observation_ms", performance["large_workload"])
        self.assertNotIn("p95_ms", performance["large_workload"])
        pilot = copy.deepcopy(payload)
        pilot["parameters"]["large_only"] = True
        pilot["workloads"] = [pilot["workloads"][-1]]
        self.assertEqual(self.checks.verify_large_payload(pilot)["name"],
                         "subdivided_cube_n300")

    def test_bitpacked_field_rejects_nonzero_padding(self):
        field = _field([0, 0, 0], [3, 3, 1], "object_kernel", {(0, 0, 0)})
        field["bits_hex"] = field["bits_hex"][:-2] + "80"
        with self.assertRaises(self.checks.PayloadError):
            self.checks._decode_field(field, "fixture", field["window"],
                                      "object_kernel", True)

    def test_rejects_reviewed_semantic_and_budget_mutations(self):
        baseline = valid_payload()

        def free_claimed_surface(payload):
            workload = payload["workloads"][0]
            cell = workload["samples"][0]["field_a"]["witnesses"][0]["cell"]
            _set_workload_field_bit(workload, "object", cell, 0)
            for run in workload["warmups"] + workload["samples"]:
                for side in ("field_a", "field_b"):
                    run[side]["witnesses"][0]["expected_bit"] = 0

        def free_unwitnessed_wall_cell(payload):
            _set_workload_field_bit(payload["workloads"][0], "container", [0, 1, 1], 0)

        mutations = {
            "self-claimed free surface": free_claimed_surface,
            "unwitnessed eroded-wall hole": free_unwitnessed_wall_cell,
            "small canonical kernel work": lambda p: p["workloads"][0]["canonical_fields"]["object"]["stats"].__setitem__("kernel_work", 1_300_000_001),
            "large A kernel work": lambda p: p["workloads"][-1]["run"]["field_a"]["fields"]["placed"]["stats"].__setitem__("kernel_work", 16_000_000_001),
            "small B cell visits": lambda p: p["workloads"][1]["samples"][0]["field_b"]["fields"]["container"]["stats"].__setitem__("cell_visits", 200_000_001),
            "small B uncertainty beyond halo": lambda p: p["workloads"][0]["samples"][0]["field_b"]["fields"]["placed"]["stats"].__setitem__("uncertain_cells", 359_425),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                payload = copy.deepcopy(baseline)
                mutate(payload)
                with self.assertRaises(self.checks.PayloadError):
                    self.checks.verify_payload(payload, EXPECTATIONS, 3, 1)

    def test_allows_bounded_raw_uncertainty_counts(self):
        payload = valid_payload()
        workload = payload["workloads"][0]
        workload["canonical_fields"]["placed"]["stats"]["uncertain_cells"] = 1
        for run in workload["warmups"] + workload["samples"]:
            for side in ("field_a", "field_b"):
                run[side]["fields"]["placed"]["stats"]["uncertain_cells"] = 1
        self.checks.verify_payload(payload, EXPECTATIONS, 3, 1)

    def test_rejects_substantive_payload_mutations(self):
        baseline = valid_payload()
        mutations = {
            "bool numeric": lambda p: p["workloads"][0]["samples"][0]["lod_a"].__setitem__("actual_triangles", True),
            "import incomplete": lambda p: p["workloads"][1]["samples"][0]["object"]["import_diagnostics"].__setitem__("topology_check", 0),
            "fractional container lost": lambda p: p["workloads"][0]["samples"][0]["container"]["accepted"]["bounds_mm"]["max"].__setitem__(0, 240.0),
            "wrong window": lambda p: p["workloads"][2]["canonical_fields"]["placed"]["window"]["shape"].__setitem__(0, 25),
            "wrong purpose": lambda p: p["workloads"][0]["canonical_fields"]["object"].__setitem__("purpose", "container_blocker"),
            "padding bit": lambda p: p["workloads"][0]["canonical_fields"]["object"].__setitem__("bits_hex", p["workloads"][0]["canonical_fields"]["object"]["bits_hex"][:-2] + "80"),
            "fingerprint": lambda p: p["workloads"][1]["canonical_fields"]["placed"].__setitem__("fingerprint", 1),
            "A/B identity": lambda p: p["workloads"][0]["samples"][0]["field_b"]["fields"]["object"].__setitem__("fingerprint", 2),
            "outside witness": lambda p: p["workloads"][0]["samples"][0]["field_a"]["witnesses"][1].__setitem__("cell", [0, 0, 0]),
            "snapshot pose": lambda p: p["workloads"][2]["samples"][0]["validation_after"]["snapshot"]["poses"][0]["translation_mm"].__setitem__(0, 0.0),
            "invalid snapshot": lambda p: p["workloads"][-1]["run"]["through_wall_validation"].__setitem__("has_snapshot", True),
            "large count": lambda p: p["workloads"][-1]["run"]["field_a"]["witnesses"][-1].__setitem__("object", 4095),
            "resource cap": lambda p: p["workloads"][-1]["run"]["settings"].__setitem__("retained_reserve_bytes", 2 * 1024**3 + 1),
            "repeated fingerprint tuple": lambda p: p["workloads"][0]["samples"][1]["run_fingerprints"].__setitem__("object", 7),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                payload = copy.deepcopy(baseline)
                mutate(payload)
                with self.assertRaises(self.checks.PayloadError):
                    self.checks.verify_payload(payload, EXPECTATIONS, 3, 1)


if __name__ == "__main__":
    unittest.main()
