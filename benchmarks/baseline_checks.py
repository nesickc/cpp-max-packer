"""Independent qualification checks for the T-006 physical AABB baseline."""

import copy
import hashlib
import math
from fractions import Fraction
from pathlib import Path

try:
    from benchmarks.representation_checks import PayloadError, strict_json
except ModuleNotFoundError:
    from representation_checks import PayloadError, strict_json


ITEMS = ("rc/items/pryanik_1.STL", "rc/items/pryanik_2.STL",
         "rc/items/ulamok_2kg_simplified.stl")
CONTAINERS = ("rc/containers/10_kg_np.stl", "rc/containers/15_kg_np_long.stl",
              "rc/containers/20_kg_np.stl", "rc/containers/30_kg_np.stl",
              "rc/containers/30_kg_np_cubic.stl", "rc/containers/5_kg_np.stl")
CHECKS = ("input", "orientation", "broad_phase", "pair_solids",
          "containment", "clearance")
UINT64_MAX = (1 << 64) - 1
WORKING_BYTES = 512 * 1024 * 1024
RESERVE_BYTES = 64 * 1024 * 1024

LIMIT_VALUES = {
    "max_orientations": 2048,
    "max_copies": 4096,
    "max_axis_cells": 1_000_000,
    "max_working_bytes": WORKING_BYTES,
    "caller_reserve_bytes": RESERVE_BYTES,
    "max_geometry_kernel_work": 1_300_000_000,
    "max_geometry_vertex_visits": 200_000_000,
    "max_validation_kernel_work": 1_300_000_000,
    "max_validation_aabb_pair_tests": 50_000_000,
}
QUERY_LIMITS = {
    "max_working_bytes": 128 * 1024 * 1024,
    "max_kernel_work": 100_000_000,
    "max_vertex_visits": 5_000_000,
}
VALIDATION_LIMITS = {
    "max_copy_count": 1_000_000,
    "max_working_bytes": WORKING_BYTES,
    "max_aabb_pair_tests": 50_000_000,
    "max_kernel_work": 1_300_000_000,
    "max_diagnostic_examples": 64,
}
RUN_STATS = {
    "candidate_evaluations", "search_passes", "orientations_started",
    "geometry_kernel_work", "geometry_vertex_visits",
    "validation_kernel_work", "validation_aabb_pair_tests",
    "tracked_working_bytes_peak", "invalid_candidates",
    "indeterminate_candidates",
}


def need(condition, message):
    if not condition:
        raise PayloadError(message)


def exact_keys(value, expected, name):
    need(isinstance(value, dict), f"{name} must be an object")
    actual = set(value)
    expected = set(expected)
    need(actual == expected,
         f"{name} fields differ: missing={sorted(expected - actual)}, "
         f"extra={sorted(actual - expected)}")
    return value


def number(value, message, low=None, high=None):
    need(type(value) in (int, float) and math.isfinite(value), message)
    if low is not None:
        need(value >= low, message)
    if high is not None:
        need(value <= high, message)
    return value


def integer(value, message, low=0, high=UINT64_MAX):
    need(type(value) is int and low <= value <= high, message)
    return value


def binary64_fraction(value):
    number(value, "grid input must be a finite non-boolean number")
    return Fraction.from_float(float(value))


def same_number(actual, expected, name):
    number(actual, f"{name} must be a finite non-boolean number")
    need(binary64_fraction(actual) == binary64_fraction(expected),
         f"{name} differs")


def vector(value, expected, name):
    need(isinstance(value, list) and len(value) == len(expected),
         f"{name} is malformed")
    for index, (actual, wanted) in enumerate(zip(value, expected)):
        same_number(actual, wanted, f"{name}[{index}]")


def bounds(value, expected, name):
    exact_keys(value, {"min", "max"}, name)
    vector(value["min"], expected["min"], f"{name}.min")
    vector(value["max"], expected["max"], f"{name}.max")
    need(all(value["min"][axis] <= value["max"][axis] for axis in range(3)),
         f"{name} is inverted")


def first_axis_translations(object_min, object_max, container_min, pair_gap,
                            wall_gap, count=8):
    values = [binary64_fraction(value) for value in
              (object_min, object_max, container_min, pair_gap, wall_gap)]
    minimum, maximum, container, pair, wall = values
    return [container + wall - minimum + index * (maximum - minimum + pair)
            for index in range(count)]


def _read_document(path, name):
    try:
        return strict_json(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise PayloadError(f"cannot read {name}: {error}") from error


def _expected_documents(path):
    expectations_path = Path(path).resolve()
    manifest_path = expectations_path.with_name("rc-manifest.json")
    expectations = _read_document(expectations_path, "import expectations")
    manifest = _read_document(manifest_path, "rc manifest")
    need(expectations.get("schema_version") == 1 and
         manifest.get("schema_version") == 1, "source oracle schema differs")
    try:
        manifest_bytes = manifest_path.read_bytes()
    except OSError as error:
        raise PayloadError(f"cannot read rc manifest: {error}") from error
    need(expectations.get("manifest_sha256") ==
         hashlib.sha256(manifest_bytes).hexdigest(),
         "import expectations do not bind the rc manifest")

    def indexed(document, name):
        records = document.get("records")
        need(isinstance(records, list) and len(records) == 10,
             f"{name} must contain ten records")
        result = {}
        for record in records:
            need(isinstance(record, dict) and type(record.get("path")) is str and
                 record["path"] not in result, f"{name} record is malformed")
            result[record["path"]] = record
        return result

    expected_records = indexed(expectations, "expectations")
    manifest_records = indexed(manifest, "manifest")
    need(set(expected_records) == set(manifest_records),
         "source oracle paths differ")
    expected = {name: record.get("expected")
                for name, record in expected_records.items()}
    need(all(isinstance(value, dict) for value in expected.values()),
         "expected import result is malformed")
    try:
        source_root = expectations_path.parents[2]
    except IndexError as error:
        raise PayloadError("source oracle path has no repository root") from error
    return expected, manifest_records, source_root


def _grid(object_bounds, container_bounds, pair=1.0, wall=1.0):
    low = [binary64_fraction(value) for value in object_bounds["min"]]
    high = [binary64_fraction(value) for value in object_bounds["max"]]
    base = [binary64_fraction(value) for value in container_bounds["min"]]
    ceiling = [binary64_fraction(value) for value in container_bounds["max"]]
    pair, wall = binary64_fraction(pair), binary64_fraction(wall)
    width = [high[i] - low[i] for i in range(3)]
    counts = [max(0, int((ceiling[i] - base[i] - 2 * wall + pair)
                         // (width[i] + pair))) for i in range(3)]
    points = []
    for z in range(counts[2]):
        for y in range(counts[1]):
            for x in range(counts[0]):
                points.append([
                    base[i] + wall - low[i]
                    + (x, y, z)[i] * (width[i] + pair)
                    for i in range(3)
                ])
    return counts, points


def _poses(points):
    return [{"copy_id": f"oracle-{index}",
             "translation_mm": [float(value) for value in point],
             "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0]}
            for index, point in enumerate(points)]


def _analytic(origin, pitch):
    return [[Fraction(origin + pitch * x), Fraction(origin + pitch * y),
             Fraction(origin + pitch * z)]
            for z in range(4) for y in range(4) for x in range(4)]


def _same_poses(actual, expected, name, bind_ids=True):
    need(isinstance(actual, list) and len(actual) == len(expected),
         f"{name} pose count differs")
    ids = set()
    for index, (pose, wanted) in enumerate(zip(actual, expected)):
        exact_keys(pose, {"copy_id", "translation_mm", "quaternion_xyzw"},
                   f"{name}[{index}]")
        copy_id = pose["copy_id"]
        need(type(copy_id) is str and copy_id and copy_id not in ids,
             f"{name} copy identities must be nonempty and unique")
        ids.add(copy_id)
        if bind_ids:
            need(copy_id == wanted["copy_id"], f"{name} copy identity differs")
        vector(pose["quaternion_xyzw"], [0.0, 0.0, 0.0, 1.0],
               f"{name}[{index}].quaternion_xyzw")
        vector(pose["translation_mm"], wanted["translation_mm"],
               f"{name}[{index}].translation_mm")


def _fnv64(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & UINT64_MAX
    return value


def _source_path(root, relative):
    need(type(relative) is str and relative, "source path is missing")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise PayloadError("source path escapes repository root") from error
    return candidate


def _solid_shape(entry, path, expected_bounds, expected_volume,
                 expected_vertices, expected_triangles):
    exact_keys(entry, {"path", "source_bytes", "source_fingerprint", "frame",
                       "accepted", "import_diagnostics"}, f"source {path}")
    need(entry["path"] == path, f"source {path} identity differs")
    integer(entry["source_bytes"], f"{path} source bytes are invalid", 1)
    integer(entry["source_fingerprint"],
            f"{path} source fingerprint is invalid", 1)
    accepted = exact_keys(
        entry["accepted"],
        {"vertices", "triangles", "bounds_mm", "volume_mm3",
         "vertex_fingerprint", "triangle_fingerprint"},
        f"{path}.accepted")
    need(integer(accepted["vertices"], f"{path} vertex count is invalid") ==
         expected_vertices, f"{path} vertex count differs")
    need(integer(accepted["triangles"], f"{path} triangle count is invalid") ==
         expected_triangles, f"{path} triangle count differs")
    bounds(accepted["bounds_mm"], expected_bounds, f"{path}.accepted.bounds_mm")
    same_number(accepted["volume_mm3"], expected_volume,
                f"{path}.accepted.volume_mm3")
    integer(accepted["vertex_fingerprint"],
            f"{path} vertex fingerprint is invalid")
    integer(accepted["triangle_fingerprint"],
            f"{path} triangle fingerprint is invalid")
    return accepted


def _rc_solid(entry, path, expected, manifest, source_root):
    counts = expected["counts"]
    _solid_shape(entry, path, expected["mesh_bounds_mm"], expected["volume_mm3"],
                 counts["vertex_count"], counts["triangle_count"])
    source = _source_path(source_root, path)
    try:
        data = source.read_bytes()
    except OSError as error:
        raise PayloadError(f"cannot read registered source {path}: {error}") from error
    need(entry["source_bytes"] == len(data), f"{path} source byte count differs")
    need(entry["source_fingerprint"] == _fnv64(data),
         f"{path} source fingerprint differs")
    need(manifest.get("bytes") == len(data) and
         manifest.get("sha256") == hashlib.sha256(data).hexdigest(),
         f"{path} differs from the rc manifest")

    source_axes = manifest.get("bounds_source_units")
    need(isinstance(source_axes, list) and len(source_axes) == 3 and
         all(isinstance(axis, list) and len(axis) == 2 for axis in source_axes),
         f"{path} source bounds are malformed")
    source_bounds = {"min": [axis[0] for axis in source_axes],
                     "max": [axis[1] for axis in source_axes]}
    role = manifest.get("role")
    need(role in {"item", "container"}, f"{path} source role differs")
    anchor = (source_bounds["min"] if role == "container" else
              [(source_bounds["min"][axis] + source_bounds["max"][axis]) / 2
               for axis in range(3)])
    dimensions = [source_bounds["max"][axis] - source_bounds["min"][axis]
                  for axis in range(3)]
    frame = exact_keys(entry["frame"],
                       {"source_bounds", "unit_scale_mm", "anchor_mm",
                        "dimensions_mm"}, f"{path}.frame")
    bounds(frame["source_bounds"], source_bounds, f"{path}.frame.source_bounds")
    same_number(frame["unit_scale_mm"], 1.0, f"{path}.frame.unit_scale_mm")
    vector(frame["anchor_mm"], anchor, f"{path}.frame.anchor_mm")
    vector(frame["dimensions_mm"], dimensions, f"{path}.frame.dimensions_mm")

    diagnostics = exact_keys(
        entry["import_diagnostics"],
        {"validity", "source_triangles", "candidate_pair_tests",
         "predicate_work", "boundary_edges", "nonmanifold_edges",
         "self_intersection_pairs", "topology_check", "intersection_check"},
        f"{path}.import_diagnostics")
    wanted = {
        "validity": 0,
        "source_triangles": counts["source_triangle_count"],
        "candidate_pair_tests": expected["work_counts"]["candidate_pair_tests"],
        "predicate_work": expected["work_counts"]["predicate_work"],
        "boundary_edges": counts["boundary_edges"],
        "nonmanifold_edges": counts["nonmanifold_edges"],
        "self_intersection_pairs": counts["self_intersection_pairs"],
        "topology_check": 1,
        "intersection_check": 1,
    }
    for key, value in wanted.items():
        need(integer(diagnostics.get(key), f"{path}.{key} is invalid") == value,
             f"{path}.{key} differs")


def _analytic_solid(entry, path, size):
    half = size / 2
    _solid_shape(entry, path,
                 {"min": [-half, -half, -half], "max": [half, half, half]},
                 size ** 3, 8, 12)
    frame = exact_keys(entry["frame"],
                       {"source_bounds", "unit_scale_mm", "anchor_mm",
                        "dimensions_mm"}, f"{path}.frame")
    bounds(frame["source_bounds"],
           {"min": [0.0, 0.0, 0.0], "max": [size, size, size]},
           f"{path}.frame.source_bounds")
    same_number(frame["unit_scale_mm"], 1.0, f"{path}.frame.unit_scale_mm")
    vector(frame["anchor_mm"], [half, half, half], f"{path}.frame.anchor_mm")
    vector(frame["dimensions_mm"], [size, size, size],
           f"{path}.frame.dimensions_mm")
    diagnostics = exact_keys(
        entry["import_diagnostics"],
        {"validity", "source_triangles", "candidate_pair_tests",
         "predicate_work", "boundary_edges", "nonmanifold_edges",
         "self_intersection_pairs", "topology_check", "intersection_check"},
        f"{path}.import_diagnostics")
    exact = {"validity": 0, "source_triangles": 12, "boundary_edges": 0,
             "nonmanifold_edges": 0, "self_intersection_pairs": 0,
             "topology_check": 1, "intersection_check": 1}
    for key, value in exact.items():
        need(integer(diagnostics.get(key), f"{path}.{key} is invalid") == value,
             f"{path}.{key} differs")
    integer(diagnostics.get("candidate_pair_tests"),
            f"{path}.candidate_pair_tests is invalid", 1)
    integer(diagnostics.get("predicate_work"),
            f"{path}.predicate_work is invalid", 1, 1_300_000_000)


def _source_map(payload, expected, manifest, source_root):
    sources = exact_keys(payload.get("sources"),
                        {"accepted_solids", "analytic_boxes"}, "sources")
    solids = sources["accepted_solids"]
    need(isinstance(solids, list) and len(solids) == 11 and
         all(isinstance(entry, dict) for entry in solids),
         "accepted source catalog is malformed")
    paths = [entry.get("path") for entry in solids]
    required = {"analytic/cube-10mm.stl", "analytic/cube-50mm.stl",
                *ITEMS, *CONTAINERS}
    need(len(set(paths)) == len(paths) and set(paths) == required,
         "accepted source set differs")
    result = {entry["path"]: entry for entry in solids}
    for path in (*ITEMS, *CONTAINERS):
        _rc_solid(result[path], path, expected[path], manifest[path], source_root)
    _analytic_solid(result["analytic/cube-10mm.stl"],
                    "analytic/cube-10mm.stl", 10.0)
    _analytic_solid(result["analytic/cube-50mm.stl"],
                    "analytic/cube-50mm.stl", 50.0)

    boxes = sources["analytic_boxes"]
    need(isinstance(boxes, list) and len(boxes) == 2 and
         all(isinstance(entry, dict) for entry in boxes),
         "analytic box catalog is malformed")
    box_paths = [entry.get("path") for entry in boxes]
    need(len(set(box_paths)) == 2 and set(box_paths) ==
         {"analytic/box-40mm", "analytic/box-45mm"},
         "analytic box set differs")
    box_map = {entry["path"]: entry for entry in boxes}
    for path, size in (("analytic/box-40mm", 40.0),
                       ("analytic/box-45mm", 45.0)):
        entry = exact_keys(box_map[path],
                           {"path", "dimensions_mm", "bounds_mm", "volume_mm3"},
                           f"box {path}")
        vector(entry["dimensions_mm"], [size, size, size],
               f"{path}.dimensions_mm")
        bounds(entry["bounds_mm"],
               {"min": [0.0, 0.0, 0.0], "max": [size, size, size]},
               f"{path}.bounds_mm")
        same_number(entry["volume_mm3"], size ** 3, f"{path}.volume_mm3")
    return result, box_map


def _limits(value, cap, passes):
    keys = {"max_candidate_evaluations", "max_search_passes", *LIMIT_VALUES,
            "per_query", "per_validation"}
    exact_keys(value, keys, "limits")
    expected = {"max_candidate_evaluations": cap,
                "max_search_passes": passes, **LIMIT_VALUES}
    for key, wanted in expected.items():
        need(integer(value.get(key), f"limit {key} is invalid") == wanted,
             f"limit {key} differs")
    query = exact_keys(value["per_query"], QUERY_LIMITS, "per_query limits")
    validation = exact_keys(value["per_validation"], VALIDATION_LIMITS,
                            "per_validation limits")
    for key, wanted in QUERY_LIMITS.items():
        need(integer(query.get(key), f"per_query {key} is invalid") == wanted,
             f"per_query {key} differs")
    for key, wanted in VALIDATION_LIMITS.items():
        need(integer(validation.get(key),
                     f"per_validation {key} is invalid") == wanted,
             f"per_validation {key} differs")
    return value


def _round_down(value):
    result = float(value)
    if Fraction.from_float(result) > value:
        result = math.nextafter(result, -math.inf)
    return result


def _round_up(value):
    result = float(value)
    if Fraction.from_float(result) < value:
        result = math.nextafter(result, math.inf)
    return result


def _expected_score(poses, object_bounds):
    if not poses:
        return 0.0, 0.0
    low = [binary64_fraction(value) for value in object_bounds["min"]]
    high = [binary64_fraction(value) for value in object_bounds["max"]]
    placed_low = [[] for _ in range(3)]
    placed_high = [[] for _ in range(3)]
    for pose in poses:
        for axis in range(3):
            translation = binary64_fraction(pose["translation_mm"][axis])
            placed_low[axis].append(_round_down(low[axis] + translation))
            placed_high[axis].append(_round_up(high[axis] + translation))
    spans = []
    for axis in range(3):
        span = (Fraction.from_float(max(placed_high[axis])) -
                Fraction.from_float(min(placed_low[axis])))
        spans.append(_round_up(span))
    xy = _round_up(Fraction.from_float(spans[0]) +
                   Fraction.from_float(spans[1]))
    return spans[2], xy


def _score(value, poses, object_bounds, name):
    score = exact_keys(value,
                       {"count", "enclosing_z_span_mm",
                        "enclosing_xy_span_sum_mm"}, name)
    need(integer(score["count"], f"{name}.count is invalid") == len(poses),
         f"{name}.count differs")
    expected_z, expected_xy = _expected_score(poses, object_bounds)
    same_number(score["enclosing_z_span_mm"], expected_z,
                f"{name}.enclosing_z_span_mm")
    same_number(score["enclosing_xy_span_sum_mm"], expected_xy,
                f"{name}.enclosing_xy_span_sum_mm")


def _volumes(value, count, object_volume, container_volume, name):
    volume = exact_keys(value,
                        {"solid_volume_mm3", "container_volume_mm3",
                         "utilization"}, name)
    same_number(volume["solid_volume_mm3"], object_volume,
                f"{name}.solid_volume_mm3")
    same_number(volume["container_volume_mm3"], container_volume,
                f"{name}.container_volume_mm3")
    used = float(count) * float(object_volume)
    expected = used / float(container_volume)
    need(math.isfinite(expected) and 0.0 <= expected <= 1.0,
         f"{name} expected utilization is invalid")
    same_number(volume["utilization"], expected, f"{name}.utilization")


def _token(value, name):
    return integer(value, f"{name} must be a positive integer token", 1)


def _validation(value, poses, context, limits):
    fresh = exact_keys(
        value,
        {"status", "source_context_identity", "fresh_context_identity",
         "validated_context_identity", "has_validated_solution",
         "reconstructed_poses", "report"}, "fresh_revalidation")
    need(fresh["status"] == "valid", "fresh revalidation is not valid")
    need(fresh["has_validated_solution"] is True,
         "fresh validated solution is missing")
    source = _token(fresh["source_context_identity"],
                    "source_context_identity")
    fresh_token = _token(fresh["fresh_context_identity"],
                         "fresh_context_identity")
    validated = _token(fresh["validated_context_identity"],
                       "validated_context_identity")
    need(source == context and fresh_token != context and validated == fresh_token,
         "fresh validation context identity differs")
    _same_poses(fresh["reconstructed_poses"], poses,
                "fresh revalidation", bind_ids=True)

    report = exact_keys(
        fresh["report"],
        {"validity", "code", "message", "epsilon_mm", "kernel_revision",
         "aabb_pair_tests", "kernel_work", "working_bytes_peak",
         "affected_copy_ids", "affected_ids_truncated", "checks"},
        "fresh report")
    need(report["validity"] == "valid" and report["code"] == "VALID" and
         type(report["message"]) is str and report["message"] and
         report["kernel_revision"] == "homogeneous-rational-interval-v2",
         "fresh report verdict differs")
    number(report["epsilon_mm"], "fresh epsilon is invalid", 0.0)
    integer(report["aabb_pair_tests"], "fresh pair count is invalid", 0,
            limits["per_validation"]["max_aabb_pair_tests"])
    integer(report["kernel_work"], "fresh kernel work is invalid", 0,
            limits["per_validation"]["max_kernel_work"])
    integer(report["working_bytes_peak"], "fresh working peak is invalid", 0,
            limits["per_validation"]["max_working_bytes"])
    need(report["affected_copy_ids"] == [] and
         report["affected_ids_truncated"] is False,
         "fresh valid report has affected copies")
    checks = report["checks"]
    need(isinstance(checks, list) and len(checks) == len(CHECKS),
         "fresh checks are incomplete")
    for index, (entry, check_name) in enumerate(zip(checks, CHECKS)):
        exact_keys(entry, {"check", "state", "method"},
                   f"fresh check {index}")
        need(entry["check"] == check_name and entry["state"] == "complete" and
             type(entry["method"]) is str and entry["method"],
             "fresh checks are incomplete")


def _snapshot(value, poses, object_bounds, object_volume, container_volume,
              revision, context, name, include_context):
    keys = {"revision", "invariant", "score", "volumes", "poses"}
    if include_context:
        keys.add("context_identity")
    snapshot = exact_keys(value, keys, name)
    need(integer(snapshot["revision"], f"{name}.revision is invalid", 1) ==
         revision, f"{name}.revision differs")
    need(snapshot["invariant"] == "best_found", f"{name}.invariant differs")
    if include_context:
        need(_token(snapshot["context_identity"], f"{name}.context_identity") ==
             context, f"{name}.context_identity differs")
    _same_poses(snapshot["poses"], poses, f"{name}.poses", bind_ids=True)
    _score(snapshot["score"], poses, object_bounds, f"{name}.score")
    _volumes(snapshot["volumes"], len(poses), object_volume, container_volume,
             f"{name}.volumes")
    return snapshot


def _run(run, phase, ordinal, oracle_poses, pass_count, object_bounds,
         object_volume, container_volume, limits):
    exact_keys(run, {"phase", "ordinal", "search_ms", "fresh_revalidation_ms",
                     "context_identity", "stats", "termination",
                     "diagnostic_code", "observations", "best_found",
                     "fresh_revalidation"}, "run")
    need(run["phase"] == phase and
         integer(run["ordinal"], "run ordinal is invalid") == ordinal,
         "run phase or ordinal differs")
    number(run["search_ms"], "search_ms is invalid", 0.0)
    number(run["fresh_revalidation_ms"],
           "fresh_revalidation_ms is invalid", 0.0)
    context = _token(run["context_identity"], "context_identity")
    need(type(run["diagnostic_code"]) is str, "diagnostic code is invalid")

    stats = exact_keys(run["stats"], RUN_STATS, "run stats")
    count = len(oracle_poses)
    need(integer(stats["candidate_evaluations"],
                 "candidate_evaluations is invalid") == count,
         "candidate evaluations differ")
    need(integer(stats["search_passes"], "search_passes is invalid") ==
         pass_count, "completed passes differ")
    need(integer(stats["orientations_started"],
                 "orientations_started is invalid") == 1,
         "orientations started differ")
    integer(stats["geometry_kernel_work"], "geometry_kernel_work is invalid",
            0, limits["max_geometry_kernel_work"])
    integer(stats["geometry_vertex_visits"],
            "geometry_vertex_visits is invalid", 0,
            limits["max_geometry_vertex_visits"])
    integer(stats["validation_kernel_work"],
            "validation_kernel_work is invalid", 0,
            limits["max_validation_kernel_work"])
    integer(stats["validation_aabb_pair_tests"],
            "validation_aabb_pair_tests is invalid", 0,
            limits["max_validation_aabb_pair_tests"])
    peak = integer(stats["tracked_working_bytes_peak"],
                   "tracked_working_bytes_peak is invalid")
    need(RESERVE_BYTES <= peak <= limits["max_working_bytes"],
         "tracked peak exceeds the solver ceiling")
    invalid = integer(stats["invalid_candidates"],
                      "invalid_candidates is invalid")
    indeterminate = integer(stats["indeterminate_candidates"],
                            "indeterminate_candidates is invalid")
    need(invalid + indeterminate <= count,
         "candidate rejection counters exceed evaluations")
    need(invalid == 0 and indeterminate == 0,
         "registered grid unexpectedly rejected a candidate")
    need(count <= limits["max_candidate_evaluations"] and
         pass_count <= limits["max_search_passes"],
         "run counters exceed search limits")

    expected_termination = "search_stalled" if count == 0 else "budget_exhausted"
    need(run["termination"] == expected_termination, "termination differs")
    history = run["observations"]
    need(isinstance(history, list) and len(history) == count + 1,
         "observation history differs")
    best_poses = run["best_found"].get("poses") \
        if isinstance(run["best_found"], dict) else None
    _same_poses(best_poses, oracle_poses, "best result oracle", bind_ids=False)
    for revision, observation in enumerate(history, 1):
        _snapshot(observation, best_poses[:revision - 1], object_bounds,
                  object_volume, container_volume, revision, context,
                  f"observation {revision}", True)
    best = _snapshot(run["best_found"], best_poses, object_bounds, object_volume,
                     container_volume, count + 1, context, "best result", False)
    final_observation = copy.deepcopy(history[-1])
    final_observation.pop("context_identity")
    need(best == final_observation,
         "best result differs from the final retained observation")
    _validation(run["fresh_revalidation"], best_poses, context, limits)
    return run["search_ms"] + run["fresh_revalidation_ms"]


def _physical_distances(poses, object_bounds, container_bounds, pair, wall):
    low = [binary64_fraction(value) for value in object_bounds["min"]]
    high = [binary64_fraction(value) for value in object_bounds["max"]]
    container_low = [binary64_fraction(value) for value in container_bounds["min"]]
    container_high = [binary64_fraction(value) for value in container_bounds["max"]]
    pair = binary64_fraction(pair)
    wall = binary64_fraction(wall)
    placed = []
    for pose in poses:
        pose_low = []
        pose_high = []
        for axis in range(3):
            translation = binary64_fraction(pose["translation_mm"][axis])
            pose_low.append(low[axis] + translation)
            pose_high.append(high[axis] + translation)
            need(pose_low[-1] - container_low[axis] >= wall and
                 container_high[axis] - pose_high[-1] >= wall,
                 "pose violates the independent wall oracle")
        placed.append((pose_low, pose_high))
    for first in range(len(placed)):
        for second in range(first):
            separated = any(
                placed[first][0][axis] - placed[second][1][axis] >= pair or
                placed[second][0][axis] - placed[first][1][axis] >= pair
                for axis in range(3))
            need(separated, "poses violate the independent pair oracle")


def verify_payload(payload, expectations_path, samples, warmup):
    """Qualify the exact registered set with independent physical oracles."""
    need(type(samples) is int and samples == 3 and
         type(warmup) is int and warmup == 1,
         "full qualification requires three samples and one warmup")
    exact_keys(payload,
               {"schema_version", "benchmark_kind", "build", "pilot",
                "setup_ms", "process_duration_ms",
                "process_peak_working_set_bytes_before",
                "process_peak_working_set_bytes_after", "sources", "workloads"},
               "payload")
    need(integer(payload["schema_version"], "schema version is invalid") == 1 and
         payload["benchmark_kind"] == "native_physical_aabb_baseline" and
         payload["pilot"] is False,
         "payload header is invalid or pilot is unqualified")
    build = exact_keys(payload["build"],
                       {"build_type", "compiler", "compiler_version"}, "build")
    need(build["build_type"] == "Release" and build["compiler"] == "msvc" and
         integer(build["compiler_version"], "compiler version is invalid", 1),
         "native build identity differs")
    setup_ms = number(payload["setup_ms"], "setup time is invalid", 0.0)
    process_ms = number(payload["process_duration_ms"],
                        "process duration is invalid", 0.0)
    need(process_ms > 0.0, "process duration must be positive")
    before = integer(payload["process_peak_working_set_bytes_before"],
                     "process peak is invalid", 1)
    after = integer(payload["process_peak_working_set_bytes_after"],
                    "process peak is invalid", before)

    expected, manifest, source_root = _expected_documents(expectations_path)
    sources, boxes = _source_map(payload, expected, manifest, source_root)
    workloads = payload["workloads"]
    need(isinstance(workloads, list) and len(workloads) == 21 and
         all(isinstance(workload, dict) for workload in workloads),
         "workload set must contain exactly 21 cases")
    names = ["cube_exact", "cube_clearance", "oversized"] + [
        f"{item}|{container}" for item in ITEMS for container in CONTAINERS]
    need([workload.get("id") for workload in workloads] == names,
         "workload identities differ")

    deterministic = []
    recorded_ms = setup_ms
    for index, workload in enumerate(workloads):
        exact_keys(workload,
                   {"id", "object_path", "container_path", "constraints",
                    "seed_order_version", "score_order_version", "thread_count",
                    "limits", "axis_counts", "axis_cell_count", "runs"},
                   f"workload {index}")
        need(integer(workload["thread_count"], "thread count is invalid", 1) == 1 and
             integer(workload["seed_order_version"],
                     "seed order version is invalid", 1) == 1 and
             integer(workload["score_order_version"],
                     "score order version is invalid", 1) == 1,
             "workload scheduling identity differs")
        if index == 0:
            points, counts, cap, passes = _analytic(5, 10), [4, 4, 4], 64, 1
            object_path, container_path = ("analytic/cube-10mm.stl",
                                           "analytic/box-40mm")
            pair, wall = 0.0, 0.0
        elif index == 1:
            points, counts, cap, passes = _analytic(6, 11), [4, 4, 4], 64, 1
            object_path, container_path = ("analytic/cube-10mm.stl",
                                           "analytic/box-45mm")
            pair, wall = 1.0, 1.0
        elif index == 2:
            points, counts, cap, passes = [], [0, 0, 0], 4096, 2048
            object_path, container_path = ("analytic/cube-50mm.stl",
                                           "analytic/box-40mm")
            pair, wall = 0.0, 0.0
        else:
            item, container = workload["id"].split("|")
            counts, points = _grid(expected[item]["mesh_bounds_mm"],
                                   expected[container]["mesh_bounds_mm"])
            cap, passes = 8, 1
            object_path, container_path = item, container
            pair, wall = 1.0, 1.0
        need(workload["object_path"] == object_path and
             workload["container_path"] == container_path,
             "workload source identity differs")
        need(isinstance(workload["axis_counts"], list) and
             len(workload["axis_counts"]) == 3, "axis counts are malformed")
        actual_counts = [integer(value, "axis count is invalid")
                         for value in workload["axis_counts"]]
        need(actual_counts == counts and
             integer(workload["axis_cell_count"], "axis cell count is invalid") ==
             math.prod(counts), "physical grid dimensions differ")

        constraints = exact_keys(
            workload["constraints"],
            {"units", "orientation_mode", "quaternion_xyzw",
             "pair_clearance_mm", "wall_clearance_mm"}, "constraints")
        need(constraints["units"] == "mm" and
             constraints["orientation_mode"] == "fixed",
             "workload constraints differ")
        vector(constraints["quaternion_xyzw"], [0.0, 0.0, 0.0, 1.0],
               "fixed quaternion")
        same_number(constraints["pair_clearance_mm"], pair, "pair clearance")
        same_number(constraints["wall_clearance_mm"], wall, "wall clearance")

        run_limits = _limits(workload["limits"], cap, passes)
        oracle_poses = _poses(points[:cap])
        object_bounds = sources[object_path]["accepted"]["bounds_mm"]
        object_volume = sources[object_path]["accepted"]["volume_mm3"]
        if container_path in boxes:
            container_bounds = boxes[container_path]["bounds_mm"]
            container_volume = boxes[container_path]["volume_mm3"]
        else:
            container_bounds = sources[container_path]["accepted"]["bounds_mm"]
            container_volume = sources[container_path]["accepted"]["volume_mm3"]
        _physical_distances(oracle_poses, object_bounds, container_bounds,
                            pair, wall)
        runs = workload["runs"]
        need(isinstance(runs, list) and len(runs) == samples + warmup,
             "run count differs")
        pass_count = 1 if len(points) <= cap else 0
        for run_index, run in enumerate(runs):
            phase = "warmup" if run_index < warmup else "sample"
            ordinal = run_index if phase == "warmup" else run_index - warmup
            recorded_ms += _run(
                run, phase, ordinal, oracle_poses, pass_count, object_bounds,
                object_volume, container_volume, run_limits)
            normalized = copy.deepcopy(run)
            for key in ("search_ms", "fresh_revalidation_ms", "context_identity",
                        "phase", "ordinal"):
                normalized.pop(key, None)
            for observation in normalized["observations"]:
                observation.pop("context_identity", None)
            for key in ("source_context_identity", "fresh_context_identity",
                        "validated_context_identity"):
                normalized["fresh_revalidation"].pop(key, None)
            deterministic.append((workload["id"], normalized))
    need(process_ms + 1e-9 >= recorded_ms,
         "process duration is shorter than recorded setup and run work")
    for name in names:
        values = [value for workload_name, value in deterministic
                  if workload_name == name]
        need(values and all(value == values[0] for value in values[1:]),
             "non-timing evidence differs across repeats")
    return {"workloads": len(workloads)}
