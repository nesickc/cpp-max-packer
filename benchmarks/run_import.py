"""Bounded AT-03/AT-04 import qualification and performance runner."""

import argparse
import bisect
from collections import Counter
import hashlib
import json
import math
import os
import platform
import statistics
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path


ROOT = Path(__file__).parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "fixtures" / "rc-manifest.json"
DEFAULT_EXPECTATIONS = ROOT / "tests" / "fixtures" / "import-expectations.json"
RESOURCE_REASONS = {"PREDICATE_WORK", "CANDIDATE_PAIR_LIMIT", "MEMORY_LIMIT"}


class HarnessError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise HarnessError(message)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def statistics_for(samples):
    require(samples and all(math.isfinite(value) and value >= 0 for value in samples),
            "timing samples must be finite and nonnegative")
    ordered = sorted(samples)
    return {
        "median_ms": statistics.median(samples),
        "p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1],
    }


def require_deadline(deadline):
    require(time.monotonic() < deadline, "overall import deadline expired")


def run_process(command, timeout_seconds, deadline):
    remaining = deadline - time.monotonic()
    require(remaining > 0, "overall import deadline expired")
    timeout = min(timeout_seconds, remaining)
    try:
        return subprocess.run(
            command, capture_output=True, text=True, encoding="utf-8",
            errors="strict", timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise HarnessError(f"inspection watchdog expired after {timeout:g} seconds") from error
    except (OSError, UnicodeError) as error:
        raise HarnessError(f"inspection process failed: {error}") from error


def paths_alias(first, second):
    first = Path(first).resolve()
    second = Path(second).resolve()
    if first == second:
        return True
    if first.exists() and second.exists():
        try:
            return os.path.samefile(first, second)
        except OSError as error:
            raise HarnessError(f"cannot compare filesystem identity: {error}") from error
    return False


def ensure_distinct_output(output, protected_paths):
    for protected in protected_paths:
        require(not paths_alias(output, protected),
                f"output aliases protected input: {protected}")


def read_json(path, description):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"cannot read {description}: {error}") from error
    require(isinstance(value, dict), f"{description} must be a JSON object")
    return value


def parse_binary_stl(path):
    data = path.read_bytes()
    require(len(data) >= 84, "binary STL is truncated")
    count = struct.unpack_from("<I", data, 80)[0]
    require(len(data) == 84 + count * 50, "binary STL length does not match triangle count")
    faces = []
    for face_index in range(count):
        offset = 84 + face_index * 50 + 12
        face = tuple(struct.unpack_from("<3f", data, offset + corner * 12)
                     for corner in range(3))
        require(all(math.isfinite(value) for point in face for value in point),
                "binary STL has non-finite coordinates")
        faces.append(face)
    return faces


def read_binary_ply(path):
    data = path.read_bytes()
    marker = b"end_header\n"
    header_end = data.find(marker)
    require(header_end >= 0, "PLY header is incomplete")
    header_end += len(marker)
    try:
        lines = data[:header_end].decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise HarnessError("PLY header is not ASCII") from error
    require(lines[:2] == ["ply", "format binary_little_endian 1.0"],
            "PLY encoding is not binary little-endian 1.0")
    require("property double x" in lines and "property double y" in lines and
            "property double z" in lines and
            "property list uchar uint vertex_indices" in lines,
            "PLY vertex or face layout is unsupported")
    try:
        vertex_count = int(next(line.split()[2] for line in lines
                                if line.startswith("element vertex ")))
        face_count = int(next(line.split()[2] for line in lines
                              if line.startswith("element face ")))
    except (StopIteration, ValueError, IndexError) as error:
        raise HarnessError("PLY element counts are malformed") from error
    require(vertex_count >= 0 and face_count >= 0, "PLY element counts are negative")
    cursor = header_end
    required_vertices = cursor + vertex_count * 24
    require(required_vertices <= len(data), "PLY vertex payload is truncated")
    vertices = [struct.unpack_from("<3d", data, cursor + index * 24)
                for index in range(vertex_count)]
    cursor = required_vertices
    faces = []
    for _ in range(face_count):
        require(cursor + 13 <= len(data), "PLY face payload is truncated")
        count, first, second, third = struct.unpack_from("<B3I", data, cursor)
        require(count == 3, "PLY contains a non-triangle face")
        require(first < vertex_count and second < vertex_count and third < vertex_count,
                "PLY face index is out of range")
        faces.append((first, second, third))
        cursor += 13
    require(cursor == len(data), "PLY contains trailing or malformed payload bytes")
    require(all(math.isfinite(value) for point in vertices for value in point),
            "PLY contains non-finite coordinates")
    return vertices, faces


def safe_artifact(report_path, relative_path):
    require(isinstance(relative_path, str) and relative_path, "artifact path is missing")
    relative = Path(relative_path)
    require(not relative.is_absolute(), "artifact path must be relative")
    base = report_path.parent.resolve()
    resolved = (base / relative).resolve()
    try:
        resolved.relative_to(base)
    except ValueError as error:
        raise HarnessError("artifact path escapes its report directory") from error
    require(resolved.is_file(), f"artifact is missing: {relative_path}")
    return resolved


def close_number(actual, expected):
    require(isinstance(actual, (int, float)) and not isinstance(actual, bool) and
            isinstance(expected, (int, float)) and not isinstance(expected, bool) and
            math.isfinite(actual) and math.isfinite(expected),
            "expected finite numeric value")
    tolerance = 16 * max(math.ulp(float(actual)), math.ulp(float(expected)), 5e-324)
    return math.isclose(float(actual), float(expected), rel_tol=0.0, abs_tol=tolerance)


def require_vector(actual, expected, message):
    require(isinstance(actual, list) and len(actual) == len(expected) and
            all(close_number(left, right) for left, right in zip(actual, expected)), message)


def unsigned_integer(value):
    return type(value) is int and value >= 0


def finite_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def validate_bounds(value, description):
    require(isinstance(value, dict), f"{description} must be an object")
    for key in ("min", "max"):
        vector = value.get(key)
        require(isinstance(vector, list) and len(vector) == 3 and
                all(finite_number(coordinate) for coordinate in vector),
                f"{description}.{key} must contain three finite numbers")
    return value


def dyadic(value):
    numerator, denominator = value.as_integer_ratio()
    return numerator, -(denominator.bit_length() - 1)


def dyadic_subtract(left, right):
    left_numerator, left_exponent = left
    right_numerator, right_exponent = right
    exponent = min(left_exponent, right_exponent)
    return (left_numerator << (left_exponent - exponent)) - \
        (right_numerator << (right_exponent - exponent)), exponent


def dyadic_multiply(left, right):
    return left[0] * right[0], left[1] + right[1]


def exact_difference_is_zero(left, right):
    return dyadic_subtract(left, right)[0] == 0


def exact_collinear(face):
    first, second, third = face
    u = [dyadic_subtract(dyadic(second[axis]), dyadic(first[axis])) for axis in range(3)]
    v = [dyadic_subtract(dyadic(third[axis]), dyadic(first[axis])) for axis in range(3)]
    return all((
        exact_difference_is_zero(dyadic_multiply(u[1], v[2]), dyadic_multiply(u[2], v[1])),
        exact_difference_is_zero(dyadic_multiply(u[2], v[0]), dyadic_multiply(u[0], v[2])),
        exact_difference_is_zero(dyadic_multiply(u[0], v[1]), dyadic_multiply(u[1], v[0])),
    ))


def retained_source_faces(source_faces):
    seen = set()
    retained = Counter()
    for face in source_faces:
        if exact_collinear(face):
            continue
        cyclic = min((face[0], face[1], face[2]),
                     (face[1], face[2], face[0]),
                     (face[2], face[0], face[1]))
        if cyclic in seen:
            continue
        seen.add(cyclic)
        retained[tuple(sorted(face))] += 1
    return retained


def snap_coordinate(value, sorted_values):
    position = bisect.bisect_left(sorted_values, value)
    candidates = sorted_values[max(0, position - 1):position + 1]
    require(candidates, "cannot reconstruct a PLY coordinate from the source STL")
    nearest = min(candidates, key=lambda candidate: abs(candidate - value))
    require(close_number(value, nearest),
            "PLY vertex does not reconstruct to a source STL coordinate")
    return nearest


def verify_geometry(source_faces, ply_vertices, ply_faces, matrix):
    scale = matrix[0][0]
    translation = [matrix[axis][3] for axis in range(3)]
    source_points = {point for face in source_faces for point in face}
    axes = [sorted({point[axis] for point in source_points}) for axis in range(3)]
    reconstructed = []
    for local in ply_vertices:
        source = tuple((local[axis] - translation[axis]) / scale for axis in range(3))
        snapped = tuple(snap_coordinate(source[axis], axes[axis]) for axis in range(3))
        require(snapped in source_points, "PLY vertex is not present in the source STL")
        reconstructed.append(snapped)
    reconstructed_faces = Counter()
    for face in ply_faces:
        key = tuple(sorted(reconstructed[index] for index in face))
        reconstructed_faces[key] += 1
    require(reconstructed_faces == retained_source_faces(source_faces),
            "PLY facet coverage or multiplicity differs from exact source cleanup")
    return reconstructed


def validate_frame(report, record):
    bounds = record["bounds_source_units"]
    expected_min = [axis[0] for axis in bounds]
    expected_max = [axis[1] for axis in bounds]
    source_bounds = report.get("frame", {}).get("source_bounds", {})
    require_vector(source_bounds.get("min"), expected_min, "source minimum bounds changed")
    require_vector(source_bounds.get("max"), expected_max, "source maximum bounds changed")
    scale = report.get("source", {}).get("unit_scale_mm")
    require(isinstance(scale, (int, float)) and not isinstance(scale, bool) and
            math.isfinite(scale) and scale > 0, "unit scale is invalid")
    require(scale == 1.0, "rc qualification requires explicit millimeters")
    role = "container" if record["role"] == "container" else "object"
    anchors = [low if role == "container" else low / 2.0 + high / 2.0
               for low, high in bounds]
    expected_matrix = [
        [scale, 0.0, 0.0, -anchors[0]],
        [0.0, scale, 0.0, -anchors[1]],
        [0.0, 0.0, scale, -anchors[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]
    matrix = report.get("frame", {}).get("source_to_local")
    require(isinstance(matrix, list) and len(matrix) == 4 and
            all(isinstance(row, list) and len(row) == 4 for row in matrix),
            "source-to-local matrix shape is invalid")
    for row in range(4):
        for column in range(4):
            require(matrix[row][column] == expected_matrix[row][column],
                    "source-to-local matrix changed")
    dimensions = [(high * scale) - (low * scale) for low, high in bounds]
    require_vector(report.get("dimensions_mm"), dimensions, "physical dimensions changed")
    return matrix


def validate_box(record, report, reconstructed, ply_faces):
    diagnostics = report["diagnostics"]["import"]
    require(report["state"] == "accepted" and report["diagnostics"]["status"] == "valid",
            "container was not accepted as valid")
    require([diagnostics[key] for key in
             ("topology_check", "intersection_check", "containment_check")] ==
            ["complete", "complete", "complete"], "container checks are incomplete")
    require(diagnostics["component_count"] == 1 and len(diagnostics["shells"]) == 1,
            "container must have one shell")
    require(len(set(reconstructed)) == 8 and len(ply_faces) == 12,
            "container is not the reviewed eight-corner, twelve-triangle box")
    bounds = record["bounds_source_units"]
    corners = {(x, y, z) for x in bounds[0] for y in bounds[1] for z in bounds[2]}
    require(set(reconstructed) == corners, "container vertices differ from reviewed box corners")
    expected_volume = math.prod(high - low for low, high in bounds)
    require(close_number(diagnostics.get("volume_mm3"), expected_volume),
            "container volume differs from the exact bounds product")


def validate_diagnostics(report):
    require(type(report.get("schema_version")) is int and report["schema_version"] == 1,
            "unsupported inspection report schema version")
    require(report.get("state") in {"accepted", "inspected"}, "inspection state is invalid")
    wrapper = report.get("diagnostics")
    require(isinstance(wrapper, dict) and wrapper.get("status") in
            {"valid", "invalid", "indeterminate"}, "diagnostic status is invalid")
    require(isinstance(wrapper.get("messages"), list) and
            all(isinstance(message, str) for message in wrapper["messages"]),
            "diagnostic messages are invalid")
    diagnostics = wrapper.get("import")
    require(isinstance(diagnostics, dict), "import diagnostics are missing")
    integer_fields = (
        "source_byte_size", "source_triangle_count", "vertex_count", "triangle_count",
        "component_count", "boundary_edges", "nonmanifold_edges",
        "nonmanifold_vertices", "zero_area_faces", "duplicate_faces",
        "self_intersection_pairs", "candidate_pair_tests", "predicate_work",
    )
    require(all(unsigned_integer(diagnostics.get(field)) for field in integer_fields),
            "diagnostic count has an invalid type or value")
    cleanup = diagnostics.get("cleanup")
    require(isinstance(cleanup, dict) and all(unsigned_integer(cleanup.get(field)) for field in (
        "exact_vertices_merged", "duplicate_faces_removed",
        "zero_area_faces_removed", "faces_reoriented")),
        "cleanup count has an invalid type or value")
    require(type(diagnostics.get("issues_truncated")) is bool,
            "issues_truncated must be Boolean")
    for field in ("topology_check", "intersection_check", "containment_check"):
        require(diagnostics.get(field) in {"not_run", "complete", "indeterminate"},
                f"{field} is invalid")
    issues = diagnostics.get("issues")
    require(isinstance(issues, list) and all(
        isinstance(issue, dict) and isinstance(issue.get("reason"), str) and
        isinstance(issue.get("message"), str) for issue in issues),
        "diagnostic issues are invalid")
    shells = diagnostics.get("shells")
    require(isinstance(shells, list), "shell records are invalid")
    for shell in shells:
        require(isinstance(shell, dict) and unsigned_integer(shell.get("id")) and
                unsigned_integer(shell.get("triangle_count")) and
                (shell.get("parent_id") is None or unsigned_integer(shell.get("parent_id"))) and
                (shell.get("depth") is None or unsigned_integer(shell.get("depth"))) and
                shell.get("input_orientation") in {"unresolved", "outward", "inward"} and
                shell.get("final_orientation") in {"unresolved", "outward", "inward"},
                "shell record is invalid")
    if "mesh_bounds_mm" in diagnostics:
        validate_bounds(diagnostics["mesh_bounds_mm"], "diagnostic mesh bounds")
    if "volume_mm3" in diagnostics:
        require(finite_number(diagnostics["volume_mm3"]), "diagnostic volume is invalid")
    return wrapper, diagnostics


def validate_inspection(record, source_path, report_path, original_bytes):
    require(source_path.read_bytes() == original_bytes, "source bytes changed during inspection")
    report = read_json(report_path, "inspection report")
    diagnostics_wrapper, diagnostics = validate_diagnostics(report)
    source_hash = sha256_bytes(original_bytes)
    require(source_hash == record.get("sha256"), "manifest source hash does not match bytes")
    require(len(original_bytes) == record.get("bytes"), "manifest source size does not match bytes")
    source = report.get("source", {})
    expected_role = "container" if record["role"] == "container" else "object"
    require(report.get("role") == expected_role, "inspection role changed")
    require(source.get("sha256") == source_hash and source.get("byte_size") == len(original_bytes),
            "report source identity changed")
    require(source.get("units") == "mm", "report units are not explicit millimeters")
    source_artifact = safe_artifact(report_path, source.get("path"))
    require(source_artifact.read_bytes() == original_bytes,
            "retained source artifact differs from original bytes")
    matrix = validate_frame(report, record)

    require(diagnostics.get("source_byte_size") == len(original_bytes) and
            diagnostics.get("source_triangle_count") == record.get("triangles"),
            "diagnostic source counts changed")
    artifact_kind = "accepted_solid" if report.get("state") == "accepted" else "preview"
    artifact = report.get(artifact_kind)
    require(isinstance(artifact, dict), "authoritative or preview mesh reference is missing")
    require(artifact.get("format") in (None, "binary_little_endian_ply_f64_u32"),
            "mesh artifact format changed")
    ply_path = safe_artifact(report_path, artifact.get("path"))
    require(sha256_file(ply_path) == artifact.get("sha256"), "PLY artifact hash changed")
    vertices, faces = read_binary_ply(ply_path)
    require(len(vertices) == diagnostics.get("vertex_count") and
            len(faces) == diagnostics.get("triangle_count"),
            "PLY and diagnostic mesh counts differ")
    if artifact_kind == "accepted_solid":
        require(len(vertices) == artifact.get("vertex_count") and
                len(faces) == artifact.get("triangle_count"),
                "PLY and accepted-solid counts differ")
    require(record.get("format") == "binary", "rc runner currently requires pinned binary STL")
    source_faces = parse_binary_stl(source_path)
    require(len(source_faces) == record.get("triangles"), "binary STL triangle count changed")
    reconstructed = verify_geometry(source_faces, vertices, faces, matrix)
    if record["role"] == "container":
        validate_box(record, report, reconstructed, faces)

    reasons = sorted({issue.get("reason") for issue in diagnostics.get("issues", [])
                      if isinstance(issue, dict) and isinstance(issue.get("reason"), str)})
    counts = {key: diagnostics.get(key) for key in (
        "source_triangle_count", "vertex_count", "triangle_count", "component_count",
        "boundary_edges", "nonmanifold_edges", "nonmanifold_vertices",
        "zero_area_faces", "duplicate_faces", "self_intersection_pairs")}
    counts["cleanup"] = diagnostics.get("cleanup")
    observed = {
        "state": report.get("state"),
        "status": diagnostics_wrapper.get("status"),
        "reasons": reasons,
        "checks": {
            "topology": diagnostics.get("topology_check"),
            "intersection": diagnostics.get("intersection_check"),
            "containment": diagnostics.get("containment_check"),
        },
        "counts": counts,
        "work_counts": {
            "candidate_pair_tests": diagnostics.get("candidate_pair_tests"),
            "predicate_work": diagnostics.get("predicate_work"),
        },
        "artifact_kind": artifact_kind,
        "artifact_sha256": artifact.get("sha256"),
        "mesh_bounds_mm": diagnostics.get("mesh_bounds_mm"),
        "volume_mm3": diagnostics.get("volume_mm3"),
        "shells": [{key: shell.get(key) for key in (
            "id", "parent_id", "depth", "triangle_count",
            "input_orientation", "final_orientation")} for shell in diagnostics["shells"]],
        "issues_truncated": diagnostics["issues_truncated"],
    }
    require(source_path.read_bytes() == original_bytes, "source bytes changed during validation")
    return observed


def require_qualifiable(observed):
    require(observed.get("status") != "indeterminate", "indeterminate outcome cannot qualify")
    require(not RESOURCE_REASONS.intersection(observed.get("reasons", [])),
            "resource-capped outcome cannot qualify")
    checks = observed.get("checks", {})
    require(all(value != "indeterminate" for value in checks.values()),
            "indeterminate check cannot qualify")
    if observed.get("status") == "valid":
        require(observed.get("state") == "accepted" and
                list(checks.values()) == ["complete", "complete", "complete"],
                "valid outcome is not completely accepted")
    elif observed.get("status") == "invalid":
        require(observed.get("state") == "inspected" and observed.get("reasons"),
                "invalid outcome lacks a decisive diagnostic")
        counts = observed.get("counts", {})
        decisive = False
        for reason in observed["reasons"]:
            if reason == "BOUNDARY_EDGE":
                decisive |= checks.get("topology") == "complete" and counts.get("boundary_edges", 0) > 0
            elif reason == "NONMANIFOLD_EDGE":
                decisive |= checks.get("topology") == "complete" and counts.get("nonmanifold_edges", 0) > 0
            elif reason == "NONMANIFOLD_VERTEX":
                decisive |= checks.get("topology") == "complete" and counts.get("nonmanifold_vertices", 0) > 0
            elif reason == "SELF_INTERSECTION":
                decisive |= (checks.get("intersection") == "complete" and
                             counts.get("self_intersection_pairs", 0) > 0)
            elif reason in {"ZERO_VOLUME_SHELL", "CONTAINMENT_CYCLE"}:
                decisive |= checks.get("containment") == "complete"
        require(decisive, "invalid outcome is not supported by its completed diagnostic stage")
    else:
        raise HarnessError("unknown inspection status")


def compare_expectation(observed, expected):
    require(isinstance(expected, dict), "fixture expectation must be an object")
    require(observed == expected, "fixture observation differs from reviewed expectation")


def load_manifest(path):
    value = read_json(path, "rc manifest")
    require(value.get("schema_version") == 1 and isinstance(value.get("records"), list),
            "rc manifest shape is invalid")
    records = value["records"]
    require(len(records) == 10, "rc manifest must contain ten pinned assets")
    require(len({record.get("path") for record in records}) == len(records),
            "rc manifest has duplicate paths")
    return records


def require_expectation_manifest(document, manifest_sha256):
    require(document.get("manifest_sha256") == manifest_sha256,
            "import expectations do not bind the pinned rc manifest")


def load_expectations(path, manifest_sha256):
    value = read_json(path, "import expectations")
    require(value.get("schema_version") == 1 and isinstance(value.get("records"), list),
            "import expectations shape is invalid")
    require_expectation_manifest(value, manifest_sha256)
    result = {}
    for record in value["records"]:
        require(isinstance(record, dict) and isinstance(record.get("path"), str),
                "import expectation record is invalid")
        require(record["path"] not in result, "import expectations have duplicate paths")
        result[record["path"]] = record.get("expected")
    return result, value


def release_metadata(metadata):
    return isinstance(metadata, dict) and metadata.get("build_type") == "Release"


def repository_metadata(deadline):
    try:
        require_deadline(deadline)
        revision_result = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
            text=True, encoding="utf-8", errors="strict",
            timeout=max(0.001, min(5.0, deadline - time.monotonic())), check=False)
        require_deadline(deadline)
        status_result = subprocess.run(
            ["git", "status", "--porcelain"], cwd=ROOT, capture_output=True,
            text=True, encoding="utf-8", errors="strict",
            timeout=max(0.001, min(5.0, deadline - time.monotonic())), check=False)
        if revision_result.returncode != 0 or status_result.returncode != 0:
            return {"revision": None, "dirty": None}
        revision = revision_result.stdout.strip()
        dirty = bool(status_result.stdout.strip())
        return {"revision": revision, "dirty": dirty}
    except (OSError, subprocess.TimeoutExpired, UnicodeError, HarnessError):
        return {"revision": None, "dirty": None}


def invoke_inspection(executable, record, source_path, report_path,
                      original_bytes, timeout_seconds, deadline, log_prefix):
    role = "container" if record["role"] == "container" else "object"
    command = [str(executable), "inspect", "--stl", str(source_path),
               "--role", role, "--units", "mm", "--report", str(report_path)]
    started = time.perf_counter()
    completed = run_process(command, timeout_seconds, deadline)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    log_prefix.with_suffix(".stdout.txt").write_text(completed.stdout, encoding="utf-8")
    log_prefix.with_suffix(".stderr.txt").write_text(completed.stderr, encoding="utf-8")
    require(completed.returncode == 0,
            f"inspection failed for {record['path']} with exit {completed.returncode}")
    try:
        summary = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise HarnessError(f"inspection stdout is not JSON for {record['path']}") from error
    require(isinstance(summary, dict) and summary.get("report_path"),
            "inspection summary is incomplete")
    require(paths_alias(Path(summary["report_path"]), report_path),
            "inspection summary names a different report")
    observed = validate_inspection(record, source_path, report_path, original_bytes)
    return observed, elapsed_ms


def write_report(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".{uuid.uuid4().hex}.tmp")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--build-metadata", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--expectations", default=str(DEFAULT_EXPECTATIONS))
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=float, default=120)
    parser.add_argument("--overall-timeout-seconds", type=float, default=600)
    parser.add_argument("--discover", action="store_true")
    args = parser.parse_args(argv)
    try:
        require(args.samples > 0 and args.warmup >= 0, "invalid sample or warmup count")
        require(math.isfinite(args.timeout_seconds) and args.timeout_seconds > 0,
                "invalid process timeout")
        require(math.isfinite(args.overall_timeout_seconds) and args.overall_timeout_seconds > 0,
                "invalid overall timeout")
        executable = Path(args.executable).resolve()
        metadata_path = Path(args.build_metadata).resolve()
        output = Path(args.output).resolve()
        manifest_path = DEFAULT_MANIFEST.resolve()
        expectations_path = Path(args.expectations).resolve()
        require(executable.is_file(), "engine executable is missing")
        require(metadata_path.is_file(), "build metadata is missing")
        records = load_manifest(manifest_path)
        sources = [(ROOT / record["path"]).resolve() for record in records]
        protected = [executable, metadata_path, manifest_path, expectations_path, *sources]
        ensure_distinct_output(output, protected)
        metadata = read_json(metadata_path, "build metadata")
        expectations = None
        expectations_document = None
        if not args.discover:
            expectations, expectations_document = load_expectations(
                expectations_path, sha256_file(manifest_path))
            require(set(expectations) == {record["path"] for record in records},
                    "expectations do not cover exactly the manifest assets")

        artifact_root = output.parent / f"{output.name}.artifacts-{uuid.uuid4().hex}"
        artifact_root.mkdir(parents=True, exist_ok=False)
        deadline = time.monotonic() + args.overall_timeout_seconds
        observations = []
        originals = {}
        for index, (record, source_path) in enumerate(zip(records, sources)):
            require(source_path.is_file(), f"rc source is missing: {record['path']}")
            original = source_path.read_bytes()
            originals[record["path"]] = original
            run_directory = artifact_root / f"asset-{index:02d}"
            report_path = run_directory / "inspection.json"
            run_directory.mkdir()
            observed, elapsed_ms = invoke_inspection(
                executable, record, source_path, report_path, original,
                args.timeout_seconds, deadline, run_directory / "inspect")
            require_deadline(deadline)
            if not args.discover:
                require_qualifiable(observed)
                compare_expectation(observed, expectations[record["path"]])
            observations.append({"path": record["path"], "expected": observed,
                                 "initial_elapsed_ms": elapsed_ms})

        largest = next(record for record in records
                       if record["path"].replace("\\", "/").endswith("pryanik_2.STL"))
        source_path = (ROOT / largest["path"]).resolve()
        reference = next(entry["expected"] for entry in observations
                         if entry["path"] == largest["path"])
        samples = []
        for index in range(args.warmup + args.samples):
            run_directory = artifact_root / f"timing-{index:02d}"
            report_path = run_directory / "inspection.json"
            run_directory.mkdir()
            observed, elapsed_ms = invoke_inspection(
                executable, largest, source_path, report_path, originals[largest["path"]],
                args.timeout_seconds, deadline, run_directory / "inspect")
            require_deadline(deadline)
            compare_expectation(observed, reference)
            if index >= args.warmup:
                samples.append(elapsed_ms)

        require_deadline(deadline)
        metadata_hash = sha256_file(metadata_path)
        expectation_hash = None if args.discover else sha256_file(expectations_path)
        qualified = (not args.discover and release_metadata(metadata))
        require(args.discover or release_metadata(metadata),
                "only a Release build can produce qualification evidence")
        report = {
            "schema_version": 1,
            "benchmark_kind": "stl_import_qualification",
            "qualified": qualified,
            "discovery": args.discover,
            "metadata": {
                "build": metadata,
                "build_metadata_sha256": metadata_hash,
                "executable_sha256": sha256_file(executable),
                "manifest_sha256": sha256_file(manifest_path),
                "expectations_sha256": expectation_hash,
                "platform": platform.platform(),
                "cpu": platform.processor(),
                "python": platform.python_version(),
                "repository": repository_metadata(deadline),
                "parameters": {
                    "samples": args.samples, "warmup": args.warmup,
                    "timeout_seconds": args.timeout_seconds,
                    "overall_timeout_seconds": args.overall_timeout_seconds,
                },
                "artifact_directory": str(artifact_root),
            },
            "records": observations,
            "performance": {
                "path": largest["path"],
                "samples_ms": samples,
                **statistics_for(samples),
                "work_counts": reference["work_counts"],
                "outcome_sha256": sha256_bytes(
                    json.dumps(reference, sort_keys=True, separators=(",", ":")).encode("utf-8")),
            },
        }
        if expectations_document is not None:
            report["expectations_schema_version"] = expectations_document["schema_version"]
        require_deadline(deadline)
        write_report(output, report)
        print(f"import qualification report: {output}")
        return 0
    except (HarnessError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
