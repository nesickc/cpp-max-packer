import copy
import importlib.util
import math
import unittest
from fractions import Fraction
from pathlib import Path


ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location(
    "baseline_checks", ROOT / "benchmarks" / "baseline_checks.py")
checks = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checks)

ITEMS = ("rc/items/pryanik_1.STL", "rc/items/pryanik_2.STL",
         "rc/items/ulamok_2kg_simplified.stl")
CONTAINERS = ("rc/containers/10_kg_np.stl", "rc/containers/15_kg_np_long.stl",
              "rc/containers/20_kg_np.stl", "rc/containers/30_kg_np.stl",
              "rc/containers/30_kg_np_cubic.stl", "rc/containers/5_kg_np.stl")
CHECK_NAMES = ("input", "orientation", "broad_phase", "pair_solids",
               "containment", "clearance")
RESERVE_BYTES = 64 * 1024 * 1024
WORKING_BYTES = 512 * 1024 * 1024


def fraction(value):
    return Fraction.from_float(float(value))


def fnv64(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def documents(expectations_path, manifest_path):
    expected_document = checks.strict_json(
        Path(expectations_path).read_text(encoding="utf-8"))
    manifest_document = checks.strict_json(
        Path(manifest_path).read_text(encoding="utf-8"))
    expected = {record["path"]: record["expected"]
                for record in expected_document["records"]}
    manifest = {record["path"]: record for record in manifest_document["records"]}
    return expected, manifest


def physical_grid(object_bounds, container_bounds, pair_gap=1, wall_gap=1):
    """Independent exact binary64 identity grid, X fastest."""
    low = [fraction(value) for value in object_bounds["min"]]
    high = [fraction(value) for value in object_bounds["max"]]
    base = [fraction(value) for value in container_bounds["min"]]
    ceiling = [fraction(value) for value in container_bounds["max"]]
    pair = Fraction(pair_gap)
    wall = Fraction(wall_gap)
    width = [high[axis] - low[axis] for axis in range(3)]
    counts = [max(0, int((ceiling[axis] - base[axis] - 2 * wall + pair)
                         // (width[axis] + pair))) for axis in range(3)]
    points = []
    for z in range(counts[2]):
        for y in range(counts[1]):
            for x in range(counts[0]):
                index = (x, y, z)
                points.append([
                    base[axis] + wall - low[axis]
                    + index[axis] * (width[axis] + pair)
                    for axis in range(3)
                ])
    return counts, points


def pose_records(points):
    return [{"copy_id": f"copy-{index}",
             "translation_mm": [float(value) for value in point],
             "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0]}
            for index, point in enumerate(points)]


def analytic_points(origin, pitch):
    return [[Fraction(origin + pitch * x), Fraction(origin + pitch * y),
             Fraction(origin + pitch * z)]
            for z in range(4) for y in range(4) for x in range(4)]


def accepted_solid(path, expected, manifest, source_root):
    data = (Path(source_root) / path).read_bytes()
    bounds_by_axis = manifest["bounds_source_units"]
    source_min = [axis[0] for axis in bounds_by_axis]
    source_max = [axis[1] for axis in bounds_by_axis]
    role = manifest["role"]
    anchor = (source_min if role == "container" else
              [(source_min[i] + source_max[i]) / 2 for i in range(3)])
    counts = expected["counts"]
    return {
        "path": path,
        "source_bytes": len(data),
        "source_fingerprint": fnv64(data),
        "frame": {
            "source_bounds": {"min": source_min, "max": source_max},
            "unit_scale_mm": 1.0,
            "anchor_mm": anchor,
            "dimensions_mm": [source_max[i] - source_min[i] for i in range(3)],
        },
        "accepted": {
            "vertices": counts["vertex_count"],
            "triangles": counts["triangle_count"],
            "bounds_mm": copy.deepcopy(expected["mesh_bounds_mm"]),
            "volume_mm3": expected["volume_mm3"],
            "vertex_fingerprint": fnv64((path + ":vertices").encode("utf-8")),
            "triangle_fingerprint": fnv64((path + ":triangles").encode("utf-8")),
        },
        "import_diagnostics": {
            "validity": 0,
            "source_triangles": counts["source_triangle_count"],
            "candidate_pair_tests": expected["work_counts"]["candidate_pair_tests"],
            "predicate_work": expected["work_counts"]["predicate_work"],
            "boundary_edges": counts["boundary_edges"],
            "nonmanifold_edges": counts["nonmanifold_edges"],
            "self_intersection_pairs": counts["self_intersection_pairs"],
            "topology_check": 1,
            "intersection_check": 1,
        },
    }


def analytic_solid(path, size):
    half = size / 2
    token = f"accepted analytic cube {size:g} mm".encode("ascii")
    return {
        "path": path,
        "source_bytes": len(token),
        "source_fingerprint": fnv64(token),
        "frame": {
            "source_bounds": {"min": [0.0, 0.0, 0.0],
                              "max": [size, size, size]},
            "unit_scale_mm": 1.0,
            "anchor_mm": [half, half, half],
            "dimensions_mm": [size, size, size],
        },
        "accepted": {
            "vertices": 8, "triangles": 12,
            "bounds_mm": {"min": [-half, -half, -half],
                          "max": [half, half, half]},
            "volume_mm3": size ** 3,
            "vertex_fingerprint": fnv64(token + b":vertices"),
            "triangle_fingerprint": fnv64(token + b":triangles"),
        },
        "import_diagnostics": {
            "validity": 0, "source_triangles": 12,
            "candidate_pair_tests": 54, "predicate_work": 26000,
            "boundary_edges": 0, "nonmanifold_edges": 0,
            "self_intersection_pairs": 0, "topology_check": 1,
            "intersection_check": 1,
        },
    }


def box_source(path, size):
    return {"path": path, "dimensions_mm": [size, size, size],
            "bounds_mm": {"min": [0.0, 0.0, 0.0],
                          "max": [size, size, size]},
            "volume_mm3": size ** 3}


def limits(candidate_cap, pass_cap):
    return {
        "max_candidate_evaluations": candidate_cap,
        "max_search_passes": pass_cap,
        "max_orientations": 2048, "max_copies": 4096,
        "max_axis_cells": 1_000_000,
        "max_working_bytes": WORKING_BYTES,
        "caller_reserve_bytes": RESERVE_BYTES,
        "max_geometry_kernel_work": 1_300_000_000,
        "max_geometry_vertex_visits": 200_000_000,
        "max_validation_kernel_work": 1_300_000_000,
        "max_validation_aabb_pair_tests": 50_000_000,
        "per_query": {"max_working_bytes": 128 * 1024 * 1024,
                      "max_kernel_work": 100_000_000,
                      "max_vertex_visits": 5_000_000},
        "per_validation": {"max_copy_count": 1_000_000,
                           "max_working_bytes": WORKING_BYTES,
                           "max_aabb_pair_tests": 50_000_000,
                           "max_kernel_work": 1_300_000_000,
                           "max_diagnostic_examples": 64},
    }


def score(poses, object_bounds):
    if not poses:
        return {"count": 0, "enclosing_z_span_mm": 0.0,
                "enclosing_xy_span_sum_mm": 0.0}
    low = [fraction(value) for value in object_bounds["min"]]
    high = [fraction(value) for value in object_bounds["max"]]
    minimum = [min(fraction(pose["translation_mm"][axis]) + low[axis]
                   for pose in poses) for axis in range(3)]
    maximum = [max(fraction(pose["translation_mm"][axis]) + high[axis]
                   for pose in poses) for axis in range(3)]
    spans = [maximum[axis] - minimum[axis] for axis in range(3)]
    return {"count": len(poses), "enclosing_z_span_mm": float(spans[2]),
            "enclosing_xy_span_sum_mm": float(spans[0] + spans[1])}


def volumes(count, object_volume, container_volume):
    utilization = (float(count) * float(object_volume)) / float(container_volume)
    return {"solid_volume_mm3": object_volume,
            "container_volume_mm3": container_volume,
            "utilization": utilization}


def validation_report(count):
    return {
        "validity": "valid", "code": "VALID",
        "message": "All requested physical checks completed.",
        "epsilon_mm": 1e-6,
        "kernel_revision": "homogeneous-rational-interval-v2",
        "aabb_pair_tests": count * (count - 1) // 2,
        "kernel_work": 1000 + count * 10,
        "working_bytes_peak": 4096 + count * 64,
        "affected_copy_ids": [], "affected_ids_truncated": False,
        "checks": [{"check": name, "state": "complete",
                    "method": ("public-input" if name == "input" else
                               "quaternion-permission" if name == "orientation" else
                               "kernel")}
                   for name in CHECK_NAMES],
    }


def run_record(workload_index, phase, ordinal, poses, object_bounds,
               object_volume, container_volume, search_passes, termination):
    context_identity = (10_000 + workload_index * 100 + ordinal
                        + (0 if phase == "warmup" else 10))
    history = []
    for admitted_count in range(len(poses) + 1):
        prefix = copy.deepcopy(poses[:admitted_count])
        history.append({
            "revision": admitted_count + 1,
            "context_identity": context_identity,
            "invariant": "best_found",
            "score": score(prefix, object_bounds),
            "volumes": volumes(admitted_count, object_volume, container_volume),
            "poses": prefix,
        })
    final = copy.deepcopy(history[-1])
    final.pop("context_identity")
    return {
        "phase": phase, "ordinal": ordinal,
        "search_ms": 0.2 + ordinal,
        "fresh_revalidation_ms": 0.3 + ordinal,
        "context_identity": context_identity,
        "stats": {
            "candidate_evaluations": len(poses),
            "search_passes": search_passes,
            "orientations_started": 1,
            "geometry_kernel_work": 100 + len(poses) * 20,
            "geometry_vertex_visits": 8 + len(poses) * 8,
            "validation_kernel_work": 2000 + len(poses) * 100,
            "validation_aabb_pair_tests": len(poses) * len(poses),
            "tracked_working_bytes_peak": RESERVE_BYTES + 4096 + len(poses) * 128,
            "invalid_candidates": 0, "indeterminate_candidates": 0,
        },
        "termination": termination,
        "diagnostic_code": "",
        "observations": history,
        "best_found": final,
        "fresh_revalidation": {
            "status": "valid",
            "source_context_identity": context_identity,
            "fresh_context_identity": context_identity + 1_000_000,
            "validated_context_identity": context_identity + 1_000_000,
            "has_validated_solution": True,
            "reconstructed_poses": copy.deepcopy(poses),
            "report": validation_report(len(poses)),
        },
    }


def workload(workload_index, workload_id, object_path, container_path, points,
             object_bounds, object_volume, container_volume, axis_counts,
             candidate_cap, pass_cap, pair_gap, wall_gap, samples, warmup):
    poses = pose_records(points[:candidate_cap])
    product = math.prod(axis_counts)
    search_passes = 1 if product <= candidate_cap else 0
    termination = "search_stalled" if product == 0 else "budget_exhausted"
    runs = []
    for ordinal in range(warmup):
        runs.append(run_record(workload_index, "warmup", ordinal, poses,
                               object_bounds, object_volume, container_volume,
                               search_passes, termination))
    for ordinal in range(samples):
        runs.append(run_record(workload_index, "sample", ordinal, poses,
                               object_bounds, object_volume, container_volume,
                               search_passes, termination))
    return {
        "id": workload_id, "object_path": object_path,
        "container_path": container_path,
        "constraints": {"units": "mm", "orientation_mode": "fixed",
                        "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0],
                        "pair_clearance_mm": float(pair_gap),
                        "wall_clearance_mm": float(wall_gap)},
        "seed_order_version": 1,
        "score_order_version": 1,
        "thread_count": 1,
        "limits": limits(candidate_cap, pass_cap),
        "axis_counts": axis_counts, "axis_cell_count": product,
        "runs": runs,
    }


def complete_payload(samples=3, warmup=1, *, expectations_path=None,
                     manifest_path=None, source_root=None):
    expectations_path = expectations_path or ROOT / "tests/fixtures/import-expectations.json"
    manifest_path = manifest_path or ROOT / "tests/fixtures/rc-manifest.json"
    source_root = source_root or ROOT
    expected, manifest = documents(expectations_path, manifest_path)
    cube10 = analytic_solid("analytic/cube-10mm.stl", 10.0)
    cube50 = analytic_solid("analytic/cube-50mm.stl", 50.0)
    accepted = [cube10, cube50]
    for path in (*ITEMS, *CONTAINERS):
        accepted.append(accepted_solid(path, expected[path], manifest[path], source_root))

    workloads = [
        workload(0, "cube_exact", cube10["path"], "analytic/box-40mm",
                 analytic_points(5, 10), cube10["accepted"]["bounds_mm"], 1000.0,
                 40.0 ** 3, [4, 4, 4], 64, 1, 0, 0, samples, warmup),
        workload(1, "cube_clearance", cube10["path"], "analytic/box-45mm",
                 analytic_points(6, 11), cube10["accepted"]["bounds_mm"], 1000.0,
                 45.0 ** 3, [4, 4, 4], 64, 1, 1, 1, samples, warmup),
        workload(2, "oversized", cube50["path"], "analytic/box-40mm", [],
                 cube50["accepted"]["bounds_mm"], 50.0 ** 3, 40.0 ** 3,
                 [0, 0, 0], 4096, 2048, 0, 0, samples, warmup),
    ]
    for item in ITEMS:
        for container in CONTAINERS:
            counts, points = physical_grid(expected[item]["mesh_bounds_mm"],
                                           expected[container]["mesh_bounds_mm"])
            index = len(workloads)
            workloads.append(workload(
                index, f"{item}|{container}", item, container, points,
                expected[item]["mesh_bounds_mm"], expected[item]["volume_mm3"],
                expected[container]["volume_mm3"], counts, 8, 1, 1, 1,
                samples, warmup))

    assert len(workloads) == 21
    return {
        "schema_version": 1,
        "benchmark_kind": "native_physical_aabb_baseline",
        "build": {"build_type": "Release", "compiler": "msvc",
                  "compiler_version": 195136252},
        "pilot": False, "setup_ms": 1.0, "process_duration_ms": 1000.0,
        "process_peak_working_set_bytes_before": 1024,
        "process_peak_working_set_bytes_after": 2048,
        "sources": {
            "accepted_solids": accepted,
            "analytic_boxes": [box_source("analytic/box-40mm", 40.0),
                               box_source("analytic/box-45mm", 45.0)],
        },
        "workloads": workloads,
    }


class BaselinePayloadTests(unittest.TestCase):
    """QA-01/AT-10 independent complete-payload and mutation checks."""

    def assert_rejected(self, payload):
        with self.assertRaises(checks.PayloadError):
            checks.verify_payload(payload, ROOT / "tests/fixtures/import-expectations.json", 3, 1)

    @staticmethod
    def every_run(payload, workload=0):
        return payload["workloads"][workload]["runs"]

    def test_complete_payload_has_exact_physical_oracles_and_is_accepted(self):
        payload = complete_payload()
        self.assertEqual(len({case["id"] for case in payload["workloads"]}), 21)
        self.assertEqual([len(case["runs"]) for case in payload["workloads"]], [4] * 21)
        exact, clearance, oversized = payload["workloads"][:3]
        self.assertEqual(exact["axis_cell_count"], 64)
        self.assertEqual(clearance["limits"]["max_candidate_evaluations"], 64)
        self.assertEqual(clearance["runs"][0]["best_found"]["volumes"]["utilization"],
                         float(Fraction(64000, 91125)))
        self.assertEqual(oversized["object_path"], "analytic/cube-50mm.stl")
        self.assertEqual(len(oversized["runs"][0]["observations"]), 1)
        self.assertEqual(oversized["runs"][0]["observations"][0]["revision"], 1)
        for case, origin, pitch in ((exact, 5, 10), (clearance, 6, 11)):
            actual = [pose["translation_mm"]
                      for pose in case["runs"][0]["best_found"]["poses"]]
            expected = [[float(value) for value in point]
                        for point in analytic_points(origin, pitch)]
            self.assertEqual(actual, expected)
            self.assertEqual([entry["revision"] for entry in case["runs"][0]["observations"]],
                             list(range(1, 66)))
        checks.verify_payload(payload, ROOT / "tests/fixtures/import-expectations.json", 3, 1)

    def test_rc_utilization_and_grid_completion_are_independent(self):
        payload = complete_payload()
        cases = {case["id"]: case for case in payload["workloads"]}
        completed = cases["rc/items/ulamok_2kg_simplified.stl|rc/containers/5_kg_np.stl"]
        self.assertEqual(completed["axis_counts"], [2, 4, 1])
        self.assertEqual(completed["runs"][0]["stats"]["search_passes"], 1)
        interrupted = cases["rc/items/pryanik_1.STL|rc/containers/10_kg_np.stl"]
        self.assertGreater(interrupted["axis_cell_count"], 8)
        self.assertEqual(interrupted["runs"][0]["stats"]["search_passes"], 0)
        source = {solid["path"]: solid for solid in payload["sources"]["accepted_solids"]}
        expected = (
            8.0 * source[interrupted["object_path"]]["accepted"]["volume_mm3"] /
            source[interrupted["container_path"]]["accepted"]["volume_mm3"])
        self.assertEqual(interrupted["runs"][0]["best_found"]["volumes"]["utilization"], expected)

    def test_rejects_workload_repeat_pose_constraint_and_source_mutations(self):
        mutations = (
            lambda p: p["workloads"].pop(),
            lambda p: p["workloads"].append(copy.deepcopy(p["workloads"][0])),
            lambda p: p["workloads"][0]["runs"].pop(),
            lambda p: p["workloads"][0]["runs"].append(
                copy.deepcopy(p["workloads"][0]["runs"][-1])),
            lambda p: p["workloads"][0]["runs"][1].update(ordinal=2),
            lambda p: p["workloads"][0]["runs"][0]["best_found"]["score"].update(
                count=63),
            lambda p: p["workloads"][0]["runs"][0]["best_found"]["poses"][63]
            ["translation_mm"].__setitem__(0, 999.0),
            lambda p: p["workloads"][1]["constraints"].update(pair_clearance_mm=0.0),
            lambda p: p["workloads"][0]["constraints"].update(quaternion_xyzw=[0, 0, 0, 2]),
            lambda p: p["workloads"][0].update(thread_count=2),
            lambda p: p["workloads"][0].update(seed_order_version=2),
            lambda p: p["sources"]["accepted_solids"][0]["accepted"].update(volume_mm3=999.0),
            lambda p: p["sources"]["accepted_solids"][2]["import_diagnostics"].update(
                validity=1),
            lambda p: p["workloads"][2].update(object_path="analytic/cube-10mm.stl"),
            lambda p: p["build"].update(compiler="clang"),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                payload = complete_payload()
                mutate(payload)
                self.assert_rejected(payload)

    def test_rejects_limits_stats_history_and_nondeterministic_evidence(self):
        mutations = (
            lambda p: p["workloads"][0]["limits"].pop("per_query"),
            lambda p: p["workloads"][0]["limits"].update(caller_reserve_bytes=0),
            lambda p: p["workloads"][0]["runs"][0]["stats"].update(candidate_evaluations=True),
            lambda p: p["workloads"][0]["runs"][0]["stats"].update(
                geometry_kernel_work="100"),
            lambda p: p["workloads"][0]["runs"][0]["stats"].update(candidate_evaluations=65),
            lambda p: p["workloads"][0]["runs"][0]["stats"].update(search_passes=0),
            lambda p: p["workloads"][3]["runs"][0]["stats"].update(
                tracked_working_bytes_peak=WORKING_BYTES + 1),
            lambda p: p["workloads"][0]["runs"][0]["observations"][20].update(revision=19),
            lambda p: p["workloads"][0]["runs"][0]["observations"][10]["poses"][0]
            ["translation_mm"].__setitem__(0, 7.0),
            lambda p: p["workloads"][0]["runs"][2]["stats"].update(geometry_kernel_work=999),
            lambda p: p["workloads"][0]["runs"][2].update(termination="search_stalled"),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                payload = complete_payload()
                mutate(payload)
                self.assert_rejected(payload)

    def test_rejects_incomplete_fresh_validation_context_or_report(self):
        mutations = (
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"].update(
                status="indeterminate"),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"].update(
                has_validated_solution=False),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"].update(
                fresh_context_identity=p["workloads"][0]["runs"][0]["context_identity"]),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"].update(
                validated_context_identity=3),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"]["report"].update(
                kernel_revision="homogeneous-rational-interval-v1"),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"]["report"].update(
                validity="invalid"),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"]["report"]["checks"].pop(),
            lambda p: p["workloads"][0]["runs"][0]["fresh_revalidation"]["reconstructed_poses"][0]
            ["translation_mm"].__setitem__(0, 7.0),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                payload = complete_payload()
                mutate(payload)
                self.assert_rejected(payload)

    def test_rejects_all_repeat_negative_or_inexact_scores_and_volumes(self):
        def negative_utilization(payload):
            for run in self.every_run(payload):
                for snapshot in (*run["observations"], run["best_found"]):
                    snapshot["volumes"]["utilization"] = -1.0

        def negative_z_span(payload):
            for run in self.every_run(payload):
                for snapshot in (*run["observations"][1:], run["best_found"]):
                    snapshot["score"]["enclosing_z_span_mm"] = -123.0

        def conservative_span_mismatch(payload):
            for run in self.every_run(payload):
                for snapshot in (*run["observations"][1:], run["best_found"]):
                    snapshot["score"]["enclosing_xy_span_sum_mm"] += 1.0

        def material_volume_mismatch(payload):
            for run in self.every_run(payload):
                for snapshot in (*run["observations"], run["best_found"]):
                    snapshot["volumes"]["solid_volume_mm3"] = 999.0

        def final_history_mismatch(payload):
            for run in self.every_run(payload):
                run["best_found"]["revision"] = 64

        for mutate in (negative_utilization, negative_z_span,
                       conservative_span_mismatch, material_volume_mismatch,
                       final_history_mismatch):
            with self.subTest(mutate=mutate.__name__):
                payload = complete_payload()
                mutate(payload)
                self.assert_rejected(payload)

    def test_rejects_empty_nested_limits_exceeded_work_and_boolean_passes(self):
        def empty_query_limits(payload):
            payload["workloads"][0]["limits"]["per_query"] = {}

        def exceeded_geometry_work(payload):
            for run in self.every_run(payload):
                run["stats"]["geometry_kernel_work"] = 1_300_000_001

        def boolean_search_passes(payload):
            for run in self.every_run(payload):
                run["stats"]["search_passes"] = True

        def inconsistent_rejections(payload):
            for run in self.every_run(payload):
                run["stats"]["invalid_candidates"] = 1

        for mutate in (empty_query_limits, exceeded_geometry_work,
                       boolean_search_passes, inconsistent_rejections):
            with self.subTest(mutate=mutate.__name__):
                payload = complete_payload()
                mutate(payload)
                self.assert_rejected(payload)

    def test_rejects_unmeasured_rc_source_identity_and_frame(self):
        payload = complete_payload()
        source = payload["sources"]["accepted_solids"][2]
        source["source_bytes"] = 1
        source["source_fingerprint"] = 0
        source["frame"]["unit_scale_mm"] = 1000.0
        self.assert_rejected(payload)

    def test_rejects_missing_fresh_identity_tokens_in_every_repeat(self):
        payload = complete_payload()
        for run in self.every_run(payload):
            fresh = run["fresh_revalidation"]
            fresh.pop("fresh_context_identity")
            fresh.pop("validated_context_identity")
        self.assert_rejected(payload)

    def test_rejects_zero_process_duration_below_recorded_work(self):
        payload = complete_payload()
        payload["process_duration_ms"] = 0.0
        self.assert_rejected(payload)

    def test_rejects_nonfinite_boolean_pilot_and_strict_json_failures(self):
        for mutate in (
            lambda p: p.update(setup_ms=math.inf),
            lambda p: p.update(process_peak_working_set_bytes_after=True),
            lambda p: p.update(pilot=True),
        ):
            payload = complete_payload()
            mutate(payload)
            self.assert_rejected(payload)
        for text in ('{"count":1,"count":2}', '{"count":1e999}'):
            with self.assertRaises(checks.PayloadError):
                checks.strict_json(text)
        with self.assertRaises(checks.PayloadError):
            checks.binary64_fraction(True)


if __name__ == "__main__":
    unittest.main()
