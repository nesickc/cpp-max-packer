"""Independent semantic checks for the T-005 native qualification payload."""

import json
import math
import re
import statistics
from pathlib import Path


FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
UINT64_MAX = (1 << 64) - 1
LARGE_CAP = 2 * 1024**3
LARGE_FIELD_WORK = 16_000_000_000
SMALL_FIELD_WORK = 1_300_000_000
MAX_CELLS = 16_777_216
MAX_CELL_VISITS = 200_000_000
SMALL_CASES = (
    ("rc/items/pryanik_1.STL", 4.0, (60, 100, 50)),
    ("rc/items/pryanik_2.STL", 4.0, (60, 100, 50)),
    ("rc/items/ulamok_2kg_simplified.stl", 10.0, (24, 40, 20)),
)
CONTAINER = "rc/containers/5_kg_np.stl"
PURPOSES = {"object": "object_kernel", "placed": "placed_pair_blocker",
            "container": "container_blocker"}
TIME_LIMIT_MS = 1_200_000.0


class PayloadError(ValueError):
    """The native result is malformed or does not meet the qualification plan."""


def _need(condition, message):
    if not condition:
        raise PayloadError(message)


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        _need(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _finite_float(text):
    value = float(text)
    _need(math.isfinite(value), f"nonfinite JSON number: {text}")
    return value


def _reject_constant(value):
    raise PayloadError(f"nonfinite JSON constant: {value}")


def strict_json(text):
    """Parse exactly one UTF-8 JSON object with duplicate/nonfinite rejection."""
    _need(isinstance(text, str), "native JSON must be text")
    try:
        value = json.loads(text, object_pairs_hook=_pairs, parse_float=_finite_float,
                           parse_constant=_reject_constant)
    except PayloadError:
        raise
    except (TypeError, ValueError, json.JSONDecodeError) as error:
        raise PayloadError(f"invalid native JSON: {error}") from error
    _need(isinstance(value, dict), "native result must be an object")
    return value


def _dict(value, name):
    _need(isinstance(value, dict), f"{name} must be an object")
    return value


def _list(value, name, length=None):
    _need(isinstance(value, list), f"{name} must be an array")
    if length is not None:
        _need(len(value) == length, f"{name} must have {length} entries")
    return value


def _integer(value, name, low=0, high=UINT64_MAX):
    _need(type(value) is int and low <= value <= high,
          f"{name} must be an integer in {low}..{high}")
    return value


def _number(value, name, low=0.0, high=None):
    _need(type(value) in (int, float) and math.isfinite(value),
          f"{name} must be a finite non-boolean number")
    _need(value >= low and (high is None or value <= high), f"{name} is out of range")
    return value


def _vec(value, name, integer=False):
    values = _list(value, name, 3)
    checker = _integer if integer else _number
    return tuple(checker(item, f"{name}[{axis}]", -9_007_199_254_740_991,
                         9_007_199_254_740_991)
                 for axis, item in enumerate(values))


def _bounds(value, name):
    value = _dict(value, name)
    low = _vec(value.get("min"), f"{name}.min")
    high = _vec(value.get("max"), f"{name}.max")
    _need(all(low[i] <= high[i] for i in range(3)), f"{name} is inverted")
    return {"min": list(low), "max": list(high)}


def _same(actual, expected, name):
    _need(actual == expected, f"{name} differs: {actual!r} != {expected!r}")


def _expectation_map(path):
    try:
        root = strict_json(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as error:
        raise PayloadError(f"cannot read import expectations: {error}") from error
    records = _list(root.get("records"), "expectations.records")
    result = {}
    for index, record in enumerate(records):
        record = _dict(record, f"expectations.records[{index}]")
        name = record.get("path")
        _need(isinstance(name, str) and name not in result, "expectation paths must be unique strings")
        result[name] = _dict(record.get("expected"), f"expectation {name}")
    for name, _, _ in SMALL_CASES:
        _need(name in result, f"missing pinned expectation for {name}")
    _need(CONTAINER in result, f"missing pinned expectation for {CONTAINER}")
    return result


def _verify_solid(value, name, expected, expected_path, role):
    solid = _dict(value, name)
    _same(solid.get("path"), expected_path, f"{name}.path")
    _integer(solid.get("source_bytes"), f"{name}.source_bytes", 1)
    _integer(solid.get("source_fingerprint"), f"{name}.source_fingerprint")
    expected_bounds = expected["mesh_bounds_mm"]
    accepted = _dict(solid.get("accepted"), f"{name}.accepted")
    counts = expected["counts"]
    _same(_integer(accepted.get("vertices"), f"{name}.accepted.vertices"),
          counts["vertex_count"], f"{name}.accepted.vertices")
    _same(_integer(accepted.get("triangles"), f"{name}.accepted.triangles"),
          counts["triangle_count"], f"{name}.accepted.triangles")
    _same(_bounds(accepted.get("bounds_mm"), f"{name}.accepted.bounds_mm"),
          expected_bounds, f"{name}.accepted.bounds_mm")
    _same(_number(accepted.get("volume_mm3"), f"{name}.accepted.volume_mm3", 0.0),
          expected["volume_mm3"], f"{name}.accepted.volume_mm3")
    _integer(accepted.get("vertex_fingerprint"), f"{name}.accepted.vertex_fingerprint")
    _integer(accepted.get("triangle_fingerprint"), f"{name}.accepted.triangle_fingerprint")

    frame = _dict(solid.get("frame"), f"{name}.frame")
    source_bounds = _bounds(frame.get("source_bounds"), f"{name}.frame.source_bounds")
    _same(_number(frame.get("unit_scale_mm"), f"{name}.frame.unit_scale_mm", 0.0),
          1.0, f"{name}.frame.unit_scale_mm")
    dimensions = _vec(frame.get("dimensions_mm"), f"{name}.frame.dimensions_mm")
    calculated_dimensions = tuple(source_bounds["max"][i] - source_bounds["min"][i]
                                  for i in range(3))
    _same(dimensions, calculated_dimensions, f"{name}.frame.dimensions_mm")
    anchor = _vec(frame.get("anchor_mm"), f"{name}.frame.anchor_mm")
    expected_anchor = (tuple(source_bounds["min"]) if role == "container" else
                       tuple((source_bounds["min"][i] + source_bounds["max"][i]) / 2
                             for i in range(3)))
    _same(anchor, expected_anchor, f"{name}.frame.anchor_mm")
    converted = {"min": [source_bounds["min"][i] - anchor[i] for i in range(3)],
                 "max": [source_bounds["max"][i] - anchor[i] for i in range(3)]}
    _same(converted, expected_bounds, f"{name}.frame conversion")

    diagnostics = _dict(solid.get("import_diagnostics"), f"{name}.import_diagnostics")
    _same(_integer(diagnostics.get("validity"), f"{name}.validity", 0, 2), 0,
          f"{name}.validity")
    exact = {"source_triangles": counts["source_triangle_count"],
             "candidate_pair_tests": expected["work_counts"]["candidate_pair_tests"],
             "predicate_work": expected["work_counts"]["predicate_work"],
             "boundary_edges": counts["boundary_edges"],
             "nonmanifold_edges": counts["nonmanifold_edges"],
             "self_intersection_pairs": counts["self_intersection_pairs"],
             "topology_check": 1, "intersection_check": 1}
    for key, wanted in exact.items():
        _same(_integer(diagnostics.get(key), f"{name}.import_diagnostics.{key}"), wanted,
              f"{name}.import_diagnostics.{key}")
    return expected_bounds


def _verify_large_solid(value):
    name = "large.object"
    solid = _dict(value, name)
    source_path = solid.get("path")
    _need(isinstance(source_path, str) and
          Path(source_path.replace("/", "\\")).name == "cube_subdivided_n300.stl",
          f"{name}.path is not the pinned generated fixture")
    _same(_integer(solid.get("source_bytes"), f"{name}.source_bytes"), 54_000_084,
          f"{name}.source_bytes")
    _integer(solid.get("source_fingerprint"), f"{name}.source_fingerprint")
    frame = _dict(solid.get("frame"), f"{name}.frame")
    _same(_bounds(frame.get("source_bounds"), f"{name}.source_bounds"),
          {"min": [0.0] * 3, "max": [300.0] * 3}, f"{name}.source_bounds")
    _same(_vec(frame.get("anchor_mm"), f"{name}.anchor_mm"), (150.0,) * 3,
          f"{name}.anchor_mm")
    _same(_vec(frame.get("dimensions_mm"), f"{name}.dimensions_mm"), (300.0,) * 3,
          f"{name}.dimensions_mm")
    _same(_number(frame.get("unit_scale_mm"), f"{name}.unit_scale_mm", 0.0), 1.0,
          f"{name}.unit_scale_mm")
    accepted = _dict(solid.get("accepted"), f"{name}.accepted")
    for key, wanted in (("vertices", 540_002), ("triangles", 1_080_000)):
        _same(_integer(accepted.get(key), f"{name}.accepted.{key}"), wanted,
              f"{name}.accepted.{key}")
    _same(_bounds(accepted.get("bounds_mm"), f"{name}.accepted.bounds_mm"),
          {"min": [-150.0] * 3, "max": [150.0] * 3}, f"{name}.accepted.bounds_mm")
    _same(_number(accepted.get("volume_mm3"), f"{name}.accepted.volume_mm3"),
          27_000_000.0, f"{name}.accepted.volume_mm3")
    _integer(accepted.get("vertex_fingerprint"), f"{name}.vertex_fingerprint")
    _integer(accepted.get("triangle_fingerprint"), f"{name}.triangle_fingerprint")
    diagnostics = _dict(solid.get("import_diagnostics"), f"{name}.import_diagnostics")
    exact = {"validity": 0, "source_triangles": 1_080_000,
             "candidate_pair_tests": 9_179_952, "predicate_work": 13_737_868_762,
             "boundary_edges": 0, "nonmanifold_edges": 0,
             "self_intersection_pairs": 0, "topology_check": 1, "intersection_check": 1}
    for key, wanted in exact.items():
        _same(_integer(diagnostics.get(key), f"{name}.import_diagnostics.{key}"), wanted,
              f"{name}.import_diagnostics.{key}")


def _fnv(cells):
    value = FNV_OFFSET
    for cell in cells:
        value = ((value ^ cell) * FNV_PRIME) & UINT64_MAX
    return value


def _cube_cells(low, high):
    return {(x, y, z) for z in range(low, high + 1)
            for y in range(low, high + 1) for x in range(low, high + 1)}


def _decode_field(value, name, expected_window, expected_purpose, require_bits):
    field = _dict(value, name)
    window = _dict(field.get("window"), f"{name}.window")
    origin = _vec(window.get("origin_mm"), f"{name}.window.origin_mm")
    pitch = _number(window.get("pitch_mm"), f"{name}.window.pitch_mm", 1e-12)
    first = _vec(window.get("first"), f"{name}.window.first", integer=True)
    shape = _list(window.get("shape"), f"{name}.window.shape", 3)
    shape = tuple(_integer(item, f"{name}.window.shape[{axis}]", 1, 1_000_000)
                  for axis, item in enumerate(shape))
    actual_window = {"origin_mm": list(origin), "pitch_mm": pitch,
                     "first": list(first), "shape": list(shape)}
    _same(actual_window, expected_window, f"{name}.window")
    _same(field.get("purpose"), expected_purpose, f"{name}.purpose")
    _same(field.get("encoding"), "bitpacked_x_fast_lsb", f"{name}.encoding")
    count = math.prod(shape)
    _need(count <= MAX_CELLS, f"{name} cell count exceeds the admitted maximum")
    if require_bits:
        bits_hex = field.get("bits_hex")
        _need(isinstance(bits_hex, str) and re.fullmatch(r"[0-9a-f]*", bits_hex) is not None,
              f"{name}.bits_hex must be lowercase hexadecimal")
        _need(len(bits_hex) == 2 * ((count + 7) // 8), f"{name}.bits_hex length differs")
        packed = bytes.fromhex(bits_hex)
        if count % 8:
            _need(packed[-1] & ~((1 << (count % 8)) - 1) == 0,
                  f"{name}.bits_hex has nonzero padding")
        cells = bytearray((packed[index // 8] >> (index % 8)) & 1
                          for index in range(count))
        _same(_integer(field.get("fingerprint"), f"{name}.fingerprint"), _fnv(cells),
              f"{name}.fingerprint")
    else:
        _need("bits_hex" not in field, f"{name} must not repeat canonical bits")
        cells = None
        _integer(field.get("fingerprint"), f"{name}.fingerprint")
    stats = _dict(field.get("stats"), f"{name}.stats")
    for key in ("working_bytes_peak", "kernel_work", "cell_visits", "occupied_cells"):
        _integer(stats.get(key), f"{name}.stats.{key}", 1)
    _integer(stats.get("uncertain_cells"), f"{name}.stats.uncertain_cells", 0)
    _need(stats["occupied_cells"] <= count, f"{name}.stats occupied count is inconsistent")
    if cells is not None:
        _same(stats["occupied_cells"], sum(cells), f"{name}.stats.occupied_cells")
    return field, cells


def _verify_field_limits(field, name, kernel_work_limit, uncertain_bound):
    stats = field["stats"]
    _need(stats["kernel_work"] <= kernel_work_limit,
          f"{name}.kernel_work exceeds the configured field limit")
    _need(stats["cell_visits"] <= MAX_CELL_VISITS,
          f"{name}.cell_visits exceeds the configured field limit")
    _need(stats["uncertain_cells"] <= min(uncertain_bound, MAX_CELLS),
          f"{name}.uncertain_cells exceeds the admitted raw grid")


def _cell_bit(cells, window, cell, name):
    first, shape = window["first"], window["shape"]
    local = [cell[i] - first[i] for i in range(3)]
    _need(all(0 <= local[i] < shape[i] for i in range(3)), f"{name} lies outside field")
    return cells[local[0] + shape[0] * (local[1] + shape[1] * local[2])]


def _whole_cell_outside(cell, pitch, bounds):
    return any((cell[i] + 1) * pitch < bounds["min"][i] or
               cell[i] * pitch > bounds["max"][i] for i in range(3))


def _verify_eroded_container_exterior(field, cells, bounds, name):
    window = field["window"]
    pitch = window["pitch_mm"]
    first, shape = window["first"], window["shape"]
    for z in range(first[2], first[2] + shape[2]):
        for y in range(first[1], first[1] + shape[1]):
            for x in range(first[0], first[0] + shape[0]):
                cell = (x, y, z)
                wholly_permitted = all(
                    cell[axis] * pitch >= bounds["min"][axis] + 1.0 and
                    (cell[axis] + 1) * pitch <= bounds["max"][axis] - 1.0
                    for axis in range(3))
                if not wholly_permitted:
                    _same(_cell_bit(cells, window, cell, f"{name}.{cell}"), 1,
                          f"{name}.{cell} eroded-wall bit")


def _verify_witnesses(value, name, fields, cells, bounds, pose, container_bounds, large):
    witnesses = _list(value, f"{name}.witnesses")
    by_kind = {}
    for witness in witnesses:
        witness = _dict(witness, f"{name}.witness")
        kind = witness.get("kind")
        _need(isinstance(kind, str) and kind not in by_kind, f"{name} witness kinds must be unique")
        by_kind[kind] = witness
    required = {"object_surface", "object_exterior_strict_whole_cell", "placed_surface",
                "placed_exterior", "container_wall", "container_interior"}
    if large:
        required |= {"large_placed_exterior", "large_exact_counts"}
    _same(set(by_kind), required, f"{name} witness kinds")
    pitch = fields["placed"]["window"]["pitch_mm"]
    required_bits = {"object_surface": 1, "placed_surface": 1,
                     "object_exterior_strict_whole_cell": 0,
                     "placed_exterior": 0, "container_wall": 1,
                     "container_interior": 0}
    for kind, field_name in (("object_surface", "object"), ("placed_surface", "placed"),
                             ("object_exterior_strict_whole_cell", "object"),
                             ("placed_exterior", "placed"), ("container_wall", "container"),
                             ("container_interior", "container")):
        witness = by_kind[kind]
        cell = _vec(witness.get("cell"), f"{name}.{kind}.cell", integer=True)
        expected_bit = _integer(witness.get("expected_bit"), f"{name}.{kind}.expected_bit", 0, 1)
        _same(expected_bit, required_bits[kind], f"{name}.{kind}.expected_bit")
        _same(_cell_bit(cells[field_name], fields[field_name]["window"], cell,
                       f"{name}.{kind}.cell"), required_bits[kind], f"{name}.{kind}.bit")
    object_surface = by_kind["object_surface"]
    _same(_integer(object_surface.get("index"), f"{name}.object_surface.index"), 0,
          f"{name}.object_surface.index")
    local = _vec(object_surface.get("local_point_mm"), f"{name}.object_surface.local_point_mm")
    _need(all(bounds["min"][i] <= local[i] <= bounds["max"][i] for i in range(3)),
          f"{name}.object_surface is outside accepted bounds")
    _same(_vec(object_surface.get("cell"), f"{name}.object_surface.cell", integer=True),
          tuple(math.floor(local[i] / pitch) for i in range(3)),
          f"{name}.object_surface cell mapping")
    placed = by_kind["placed_surface"]
    _same(_integer(placed.get("index"), f"{name}.placed_surface.index"), 0,
          f"{name}.placed_surface.index")
    _same(_vec(placed.get("local_point_mm"), f"{name}.placed_surface.local_point_mm"),
          local, f"{name}.placed_surface.local_point_mm")
    physical = _vec(placed.get("physical_point_mm"), f"{name}.placed_surface.physical_point_mm")
    _same(physical, tuple(local[i] + pose[i] for i in range(3)),
          f"{name}.placed_surface physical mapping")
    _same(_vec(placed.get("cell"), f"{name}.placed_surface.cell", integer=True),
          tuple(math.floor(physical[i] / pitch) for i in range(3)),
          f"{name}.placed_surface cell mapping")
    exterior = by_kind["object_exterior_strict_whole_cell"]
    _same(_bounds(exterior.get("enclosure_bounds_mm"), f"{name}.exterior.bounds"), bounds,
          f"{name}.exterior.bounds")
    exterior_cell = _vec(exterior.get("cell"), f"{name}.exterior.cell", integer=True)
    _need(_whole_cell_outside(exterior_cell, pitch, bounds),
          f"{name}.object exterior cell is not strictly outside the AABB")
    physical_bounds = {"min": [bounds["min"][i] + pose[i] for i in range(3)],
                       "max": [bounds["max"][i] + pose[i] for i in range(3)]}
    placed_exterior = _vec(by_kind["placed_exterior"].get("cell"),
                           f"{name}.placed_exterior.cell", integer=True)
    _need(_whole_cell_outside(placed_exterior, pitch, physical_bounds),
          f"{name}.placed exterior cell is not strictly outside the AABB")
    wall_cell = _vec(by_kind["container_wall"].get("cell"),
                     f"{name}.container_wall.cell", integer=True)
    interior_cell = _vec(by_kind["container_interior"].get("cell"),
                         f"{name}.container_interior.cell", integer=True)
    _need(not all(wall_cell[i] * pitch >= container_bounds["min"][i] + 1 and
                  (wall_cell[i] + 1) * pitch <= container_bounds["max"][i] - 1
                  for i in range(3)), f"{name}.container wall cell is wholly permitted")
    _need(all(interior_cell[i] * pitch >= container_bounds["min"][i] + 1 and
              (interior_cell[i] + 1) * pitch <= container_bounds["max"][i] - 1
              for i in range(3)), f"{name}.container interior cell is not wholly permitted")
    if large:
        large_ext = by_kind["large_placed_exterior"]
        cell = _vec(large_ext.get("cell"), f"{name}.large_placed_exterior.cell", integer=True)
        _same(_integer(large_ext.get("expected_bit"), f"{name}.large exterior bit", 0, 1), 0,
              f"{name}.large exterior expected bit")
        _same(_cell_bit(cells["placed"], fields["placed"]["window"], cell,
                       f"{name}.large exterior"), 0, f"{name}.large exterior bit")
        exact = by_kind["large_exact_counts"]
        for key, wanted in (("object", 4096), ("placed", 5832), ("container", 2168)):
            _same(_integer(exact.get(key), f"{name}.large_exact_counts.{key}"), wanted,
                  f"{name}.large_exact_counts.{key}")
            _same(sum(cells[key]), wanted, f"{name}.{key} analytical occupied count")


def _verify_report(value, name, validity):
    report = _dict(value, name)
    _same(_integer(report.get("validity"), f"{name}.validity", 0, 2), validity,
          f"{name}.validity")
    _need(isinstance(report.get("code"), str) and report["code"], f"{name}.code missing")
    _need(isinstance(report.get("message"), str), f"{name}.message missing")
    _number(report.get("epsilon_mm"), f"{name}.epsilon_mm", 0.0)
    _same(report.get("kernel_revision"), "homogeneous-rational-interval-v1",
          f"{name}.kernel_revision")
    for key in ("aabb_pair_tests", "kernel_work", "working_bytes_peak"):
        _integer(report.get(key), f"{name}.{key}")
    _need(type(report.get("affected_ids_truncated")) is bool,
          f"{name}.affected_ids_truncated must be boolean")
    ids = _list(report.get("affected_copy_ids"), f"{name}.affected_copy_ids")
    _need(all(isinstance(item, str) for item in ids), f"{name}.affected_copy_ids invalid")
    checks = _list(report.get("checks"), f"{name}.checks", 6)
    states = []
    for index, check in enumerate(checks):
        check = _dict(check, f"{name}.checks[{index}]")
        _same(_integer(check.get("check"), f"{name}.checks[{index}].check", 0, 5), index,
              f"{name}.checks[{index}].check")
        states.append(_integer(check.get("state"), f"{name}.checks[{index}].state", 0, 2))
        _need(isinstance(check.get("method"), str) and check["method"],
              f"{name}.checks[{index}].method missing")
    if validity == 0:
        _same(report["code"], "VALID", f"{name}.code")
        _same(states, [1] * 6, f"{name}.completed check states")
        _same(ids, [], f"{name}.affected_copy_ids")
        _need(not report["affected_ids_truncated"], f"{name} valid IDs are truncated")
    return report, states


def _verify_validation(value, name, pose):
    result = _dict(value, name)
    report, _ = _verify_report(result.get("report"), f"{name}.report", 0)
    _need(result.get("has_snapshot") is True, f"{name} valid result lacks snapshot")
    snapshot = _dict(result.get("snapshot"), f"{name}.snapshot")
    _integer(snapshot.get("context_address"), f"{name}.snapshot.context_address", 1)
    poses = _list(snapshot.get("poses"), f"{name}.snapshot.poses", 1)
    recorded = _dict(poses[0], f"{name}.snapshot.poses[0]")
    _same(recorded.get("id"), "known-valid", f"{name}.snapshot pose id")
    _same(_vec(recorded.get("translation_mm"), f"{name}.translation_mm"), tuple(pose),
          f"{name}.translation_mm")
    rotation = _list(recorded.get("rotation_xyzw"), f"{name}.rotation_xyzw", 4)
    rotation = [_number(item, f"{name}.rotation_xyzw[{index}]")
                for index, item in enumerate(rotation)]
    _same(rotation, [0.0, 0.0, 0.0, 1.0], f"{name}.rotation_xyzw")
    snapshot_report, _ = _verify_report(snapshot.get("report"),
                                        f"{name}.snapshot.report", 0)
    _same(snapshot_report, report, f"{name}.snapshot.report")


def _verify_lod(value, name, setting, source_triangles, cap):
    lod = _dict(value, name)
    _same(_integer(lod.get("source_triangles"), f"{name}.source_triangles"),
          source_triangles, f"{name}.source_triangles")
    actual = _integer(lod.get("actual_triangles"), f"{name}.actual_triangles", 1,
                      source_triangles)
    requested = _number(lod.get("requested_error_mm"), f"{name}.requested_error_mm")
    _same(requested, setting["max_error_mm"], f"{name}.requested_error_mm")
    approximate = _number(lod.get("approximate_error_mm"), f"{name}.approximate_error_mm")
    conversion = _number(lod.get("coordinate_conversion_error_mm"),
                         f"{name}.coordinate_conversion_error_mm")
    _need(approximate <= requested and conversion <= requested,
          f"{name} exceeded requested display error")
    _need(type(lod.get("target_reached")) is bool, f"{name}.target_reached must be boolean")
    _same(lod["target_reached"], actual <= setting["target_triangles"],
          f"{name}.target_reached")
    _integer(lod.get("vertex_fingerprint"), f"{name}.vertex_fingerprint")
    _integer(lod.get("triangle_fingerprint"), f"{name}.triangle_fingerprint")
    peak = _integer(lod.get("tracked_peak_bytes"), f"{name}.tracked_peak_bytes", 1)
    _need(peak <= cap, f"{name}.tracked_peak_bytes exceeds representation cap")


def _timing(value, name):
    return float(_number(value, name, 0.0, TIME_LIMIT_MS))


def _phase_times(run, name):
    result = {}
    for key in ("import_setup_ms", "lod_a_ms", "lod_b_ms", "validation_a_ms",
                "validation_b_ms"):
        result[key] = _timing(run.get(key), f"{name}.{key}")
    for side in ("field_a", "field_b"):
        timings = _dict(_dict(run.get(side), f"{name}.{side}").get("timings"),
                        f"{name}.{side}.timings")
        for key in ("prepare_ms", "object_field_ms", "placed_field_ms",
                    "container_field_ms"):
            result[f"{side}.{key}"] = _timing(timings.get(key), f"{name}.{side}.{key}")
    return result


def _expected_object_window(bounds, pitch):
    first = [math.floor(bounds["min"][axis] / pitch) - 1 for axis in range(3)]
    last = [math.floor(bounds["max"][axis] / pitch) + 1 for axis in range(3)]
    return {"origin_mm": [0.0, 0.0, 0.0], "pitch_mm": pitch, "first": first,
            "shape": [last[axis] - first[axis] + 1 for axis in range(3)]}


def _verify_run(run_value, name, bounds, pitch, environment_shape, source_triangles,
                pose, container_bounds, settings_b, cap, canonical, large=False,
                enumerate_container_exterior=False):
    run = _dict(run_value, name)
    settings = _dict(run.get("settings"), f"{name}.settings")
    _same(_number(settings.get("pitch_mm"), f"{name}.pitch_mm", 0.0), pitch,
          f"{name}.pitch_mm")
    expected_environment = {"origin_mm": [0.0, 0.0, 0.0], "first": [0, 0, 0],
                            "shape": list(environment_shape)}
    window = _dict(settings.get("window"), f"{name}.settings.window")
    _same({"origin_mm": list(_vec(window.get("origin_mm"), f"{name}.origin")),
           "first": list(_vec(window.get("first"), f"{name}.first", integer=True)),
           "shape": list(_vec(window.get("shape"), f"{name}.shape", integer=True))},
          expected_environment, f"{name}.settings.window")
    for key in ("pair_clearance_mm", "wall_clearance_mm"):
        _same(_number(settings.get(key), f"{name}.{key}"), 1.0, f"{name}.{key}")
    setting_a = {"target_triangles": 20_000, "max_error_mm": 0.1}
    for key, wanted in (("requested_lod_a", setting_a), ("requested_lod_b", settings_b)):
        setting = _dict(settings.get(key), f"{name}.{key}")
        _same(_integer(setting.get("target_triangles"), f"{name}.{key}.target_triangles", 1),
              wanted["target_triangles"], f"{name}.{key}.target_triangles")
        _same(_number(setting.get("max_error_mm"), f"{name}.{key}.max_error_mm"),
              wanted["max_error_mm"], f"{name}.{key}.max_error_mm")
    _same(_integer(settings.get("representation_cap_bytes"), f"{name}.cap", 1), cap,
          f"{name}.representation_cap_bytes")
    reserve = _integer(settings.get("retained_reserve_bytes"), f"{name}.reserve", 1)
    _need(reserve <= cap, f"{name}.retained reserve exceeds cap")
    expected_import = ({"max_predicate_work": 40_000_000_000,
                        "max_candidate_pairs": 50_000_000} if large else {})
    expected_representation = ({"max_kernel_work": LARGE_FIELD_WORK,
                                "max_working_bytes": LARGE_CAP} if large else {})
    _same(settings.get("large_import_overrides"), expected_import,
          f"{name}.large_import_overrides")
    _same(settings.get("large_representation_overrides"), expected_representation,
          f"{name}.large_representation_overrides")
    _verify_lod(run.get("lod_a"), f"{name}.lod_a", setting_a, source_triangles, cap)
    _verify_lod(run.get("lod_b"), f"{name}.lod_b", settings_b, source_triangles, cap)

    expected_windows = {"object": _expected_object_window(bounds, pitch),
                        "placed": {**expected_environment, "pitch_mm": pitch},
                        "container": {**expected_environment, "pitch_mm": pitch}}
    halo = math.ceil(1.0 / pitch) + 1
    raw_cell_bounds = {
        "object": math.prod(expected_windows["object"]["shape"]),
        "placed": math.prod(extent + 2 * halo for extent in environment_shape),
        "container": (math.prod(environment_shape) if large else
                      math.prod(extent + 2 * halo for extent in environment_shape)),
    }
    kernel_work_limit = LARGE_FIELD_WORK if large else SMALL_FIELD_WORK
    canonical_fields, canonical_cells = {}, {}
    for field_name in PURPOSES:
        field, cells = _decode_field(canonical[field_name], f"{name}.canonical.{field_name}",
                                     expected_windows[field_name], PURPOSES[field_name], True)
        _need(field["stats"]["working_bytes_peak"] <= cap,
              f"{name}.canonical.{field_name} tracked storage exceeds cap")
        _verify_field_limits(field, f"{name}.canonical.{field_name}", kernel_work_limit,
                             raw_cell_bounds[field_name])
        canonical_fields[field_name], canonical_cells[field_name] = field, cells
    if enumerate_container_exterior:
        _verify_eroded_container_exterior(canonical_fields["container"],
                                          canonical_cells["container"], container_bounds,
                                          f"{name}.canonical.container")
    for side in ("field_a", "field_b"):
        block = _dict(run.get(side), f"{name}.{side}")
        fields = _dict(block.get("fields"), f"{name}.{side}.fields")
        _same(set(fields), set(PURPOSES), f"{name}.{side}.field names")
        for field_name in PURPOSES:
            observed, _ = _decode_field(fields[field_name], f"{name}.{side}.{field_name}",
                                        expected_windows[field_name], PURPOSES[field_name], False)
            _verify_field_limits(observed, f"{name}.{side}.{field_name}",
                                 kernel_work_limit, raw_cell_bounds[field_name])
            for key in ("window", "purpose", "encoding", "fingerprint"):
                _same(observed.get(key), canonical_fields[field_name].get(key),
                      f"{name}.{side}.{field_name}.{key}")
            observed_stats = observed["stats"]
            _same(observed_stats["occupied_cells"], sum(canonical_cells[field_name]),
                  f"{name}.{side}.{field_name}.occupied_cells")
        _verify_witnesses(block.get("witnesses"), f"{name}.{side}", canonical_fields,
                          canonical_cells, bounds, pose, container_bounds, large)
    fingerprints = _dict(run.get("run_fingerprints"), f"{name}.run_fingerprints")
    _same(set(fingerprints), set(PURPOSES), f"{name}.fingerprint tuple names")
    for field_name in PURPOSES:
        _same(_integer(fingerprints[field_name], f"{name}.fingerprints.{field_name}"),
              canonical_fields[field_name]["fingerprint"],
              f"{name}.fingerprints.{field_name}")
    _verify_validation(run.get("validation_before"), f"{name}.validation_before", pose)
    _verify_validation(run.get("validation_after"), f"{name}.validation_after", pose)
    _same(run.get("validation_after"), run.get("validation_before"),
          f"{name} validation report/snapshot/context/poses across LODs")
    for side in ("field_a", "field_b"):
        for field_name in PURPOSES:
            _need(run[side]["fields"][field_name]["stats"]["working_bytes_peak"] <= cap,
                  f"{name}.{side}.{field_name} tracked storage exceeds cap")
    return _phase_times(run, name), tuple(fingerprints[key] for key in PURPOSES)


def _summary(samples):
    result = {}
    for phase in samples[0]:
        raw = [sample[phase] for sample in samples]
        ordered = sorted(raw)
        rank = max(1, math.ceil(0.95 * len(ordered)))
        result[phase] = {"raw_ms": raw, "median_ms": float(statistics.median(raw)),
                         "p95_ms": ordered[rank - 1]}
    return result


def _verify_small_workload(workload, case, expectations, samples, warmup, repo_root):
    name, pitch, shape = case
    workload = _dict(workload, f"workload {name}")
    _same(workload.get("name"), name, f"workload {name}.name")
    warmups = _list(workload.get("warmups"), f"{name}.warmups", warmup)
    measured = _list(workload.get("samples"), f"{name}.samples", samples)
    canonical = _dict(workload.get("canonical_fields"), f"{name}.canonical_fields")
    _same(set(canonical), set(PURPOSES), f"{name}.canonical field names")
    expected = expectations[name]
    container_expected = expectations[CONTAINER]
    bounds = expected["mesh_bounds_mm"]
    container_bounds = container_expected["mesh_bounds_mm"]
    pose = [(container_bounds["min"][axis] + container_bounds["max"][axis] -
             bounds["min"][axis] - bounds["max"][axis]) / 2 for axis in range(3)]
    all_fingerprints, measured_times = [], []
    for index, run in enumerate(warmups + measured):
        prefix = f"{name}.run[{index}]"
        object_path = (repo_root / Path(name)).as_posix()
        container_path = (repo_root / Path(CONTAINER)).as_posix()
        _verify_solid(run.get("object"), f"{prefix}.object", expected, object_path, "object")
        _verify_solid(run.get("container"), f"{prefix}.container", container_expected,
                      container_path, "container")
        times, fingerprints = _verify_run(
            run, prefix, bounds, pitch, shape, expected["counts"]["triangle_count"], pose,
            container_bounds, {"target_triangles": 2000, "max_error_mm": 0.5},
            512 * 1024**2, canonical, enumerate_container_exterior=index == 0)
        all_fingerprints.append(fingerprints)
        if index >= warmup:
            measured_times.append(times)
    _need(len(set(all_fingerprints)) == 1, f"{name} repeated run field identities differ")
    return {"name": name, "phases": _summary(measured_times)}


def _verify_large_workload(workload):
    workload = _dict(workload, "large workload")
    _same(workload.get("name"), "subdivided_cube_n300", "large workload name")
    expected = _dict(workload.get("expected"), "large.expected")
    for key, wanted in (("triangles", 1_080_000), ("vertices", 540_002),
                        ("source_bytes", 54_000_084)):
        _same(_integer(expected.get(key), f"large.expected.{key}"), wanted,
              f"large.expected.{key}")
    _same(_number(expected.get("volume_mm3"), "large.expected.volume_mm3"),
          27_000_000.0, "large.expected.volume_mm3")
    run = _dict(workload.get("run"), "large.run")
    _verify_large_solid(run.get("object"))
    canonical = _dict(_dict(run.get("field_a"), "large.field_a").get("fields"),
                      "large.field_a.fields")
    # The large single observation retains its only complete field bytes here.
    shadow = dict(run)
    shadow["field_a"] = dict(run["field_a"])
    shadow["field_a"]["fields"] = {
        key: {subkey: subvalue for subkey, subvalue in value.items() if subkey != "bits_hex"}
        for key, value in canonical.items()}
    times, _ = _verify_run(shadow, "large.run", {"min": [-150.0] * 3,
                                                  "max": [150.0] * 3},
                           20.0, (20, 20, 20), 1_080_000, (200.0, 200.0, 200.0),
                           {"min": [0.0] * 3, "max": [400.0] * 3},
                           {"target_triangles": 1000, "max_error_mm": 0.01},
                           LARGE_CAP, canonical, large=True)
    through_wall = _dict(run.get("through_wall_validation"), "large.through_wall")
    report, states = _verify_report(through_wall.get("report"),
                                    "large.through_wall.report", 1)
    _need(through_wall.get("has_snapshot") is False and "snapshot" not in through_wall,
          "through-wall invalid result must not contain a snapshot")
    _need(report["code"] != "VALID" and "through-wall" in report["affected_copy_ids"],
          "through-wall rejection does not identify the rejected copy")
    _same(states[:5], [1] * 5, "through-wall completed checks through containment")
    _need(states[5] in (0, 1), "through-wall clearance check state is invalid")
    expected_object = _cube_cells(-8, 7)
    expected_placed = _cube_cells(1, 18)
    expected_container = {(x, y, z) for z in range(20) for y in range(20) for x in range(20)
                          if x in (0, 19) or y in (0, 19) or z in (0, 19)}
    expected_masks = {"object": expected_object, "placed": expected_placed,
                      "container": expected_container}
    for field_name, occupied in expected_masks.items():
        field, cells = _decode_field(canonical[field_name], f"large.oracle.{field_name}",
                                     canonical[field_name]["window"], PURPOSES[field_name], True)
        window = field["window"]
        actual = set()
        for index, bit in enumerate(cells):
            if bit:
                x = index % window["shape"][0]
                yz = index // window["shape"][0]
                y = yz % window["shape"][1]
                z = yz // window["shape"][1]
                actual.add((x + window["first"][0], y + window["first"][1],
                            z + window["first"][2]))
        _same(actual, occupied, f"large coordinatewise {field_name} mask")
    return {"name": "subdivided_cube_n300", "observation_ms": times}


def _verify_header(payload):
    payload = _dict(payload, "payload")
    _same(_integer(payload.get("schema_version"), "schema_version", 1, 1), 1,
          "schema_version")
    _same(payload.get("benchmark_kind"), "native_representation_qualification",
          "benchmark_kind")
    build = _dict(payload.get("build"), "build")
    _same(build.get("compiler"), "msvc", "build.compiler")
    _integer(build.get("compiler_version"), "build.compiler_version", 1)
    _same(build.get("build_type"), "Release", "build.build_type")
    before = _integer(payload.get("process_peak_working_set_bytes_before"), "RSS before", 1)
    after = _integer(payload.get("process_peak_working_set_bytes_after"), "RSS after", 1)
    _need(after >= before, "process peak working set decreased")
    return payload


def verify_large_payload(payload):
    """Inspect a real `--large-only` pilot without claiming full qualification."""
    payload = _verify_header(payload)
    params = _dict(payload.get("parameters"), "parameters")
    _need(params.get("large_only") is True, "large pilot is not marked large_only")
    workloads = _list(payload.get("workloads"), "workloads", 1)
    return _verify_large_workload(workloads[0])


def verify_payload(payload, expectations_path, samples, warmup):
    """Verify the exact four-workload qualification and return timing summaries."""
    samples = _integer(samples, "requested samples", 3, 20)
    warmup = _integer(warmup, "requested warmup", 1, 10)
    payload = _verify_header(payload)
    params = _dict(payload.get("parameters"), "parameters")
    _same(_integer(params.get("samples"), "parameters.samples", 3, 20), samples,
          "parameters.samples")
    _same(_integer(params.get("warmup"), "parameters.warmup", 1, 10), warmup,
          "parameters.warmup")
    _need(params.get("large_only") is False, "full qualification must not be large_only")
    _same(_integer(params.get("large_representation_cap_bytes"),
                   "parameters.large_representation_cap_bytes", 1), LARGE_CAP,
          "parameters.large_representation_cap_bytes")
    workloads = _list(payload.get("workloads"), "workloads", 4)
    expected_names = [case[0] for case in SMALL_CASES] + ["subdivided_cube_n300"]
    _same([_dict(item, "workload").get("name") for item in workloads], expected_names,
          "workload names/order/cardinality")
    expectations_file = Path(expectations_path).resolve()
    expectations = _expectation_map(expectations_file)
    repo_root = expectations_file.parents[2]
    small = [_verify_small_workload(workloads[index], case, expectations, samples, warmup,
                                    repo_root)
             for index, case in enumerate(SMALL_CASES)]
    large = _verify_large_workload(workloads[-1])
    return {"small_workloads": small, "large_workload": large}
