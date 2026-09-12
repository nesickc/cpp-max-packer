"""Independent qualifier for the fixed 91-workload validation benchmark."""

import argparse
import json
import math
import os
import platform
import re
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import NamedTuple

try:
    from benchmarks.run_import import sha256_file, statistics_for
except ModuleNotFoundError:  # Direct execution places benchmarks/ on sys.path.
    from run_import import sha256_file, statistics_for


ROOT = Path(__file__).parents[1]
MANIFEST = ROOT / "tests" / "fixtures" / "rc-manifest.json"
EXPECT = ROOT / "tests" / "fixtures" / "import-expectations.json"
ITEM_PATHS = [
    "rc/items/pryanik_1.STL",
    "rc/items/pryanik_2.STL",
    "rc/items/ulamok_2kg_simplified.stl",
]
CONTAINER_PATHS = [
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
UINT64_MAX = (1 << 64) - 1


class HarnessError(ValueError):
    pass


class SourceBinding(NamedTuple):
    relative_path: str
    path: Path
    size: int
    sha256: str


def require(condition, message):
    if not condition:
        raise HarnessError(message)


def is_finite_number(value):
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except (OverflowError, TypeError, ValueError):
        return False


def is_nonnegative_uint64(value):
    return type(value) is int and 0 <= value <= UINT64_MAX


def require_exact_keys(value, expected, description):
    require(isinstance(value, dict), f"{description} must be an object")
    actual = set(value)
    expected = set(expected)
    require(actual == expected,
            f"{description} fields differ: missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}")


def read_json(path, description):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"cannot read {description}: {error}") from error
    require(isinstance(value, dict), f"{description} must be a JSON object")
    return value


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


def require_deadline(deadline):
    require(time.monotonic() < deadline,
            "overall validation qualification deadline expired")


def safe_source_path(root, relative_path):
    require(type(relative_path) is str and relative_path,
            "manifest source path is missing")
    relative = Path(relative_path)
    require(not relative.is_absolute(), "manifest source path must be relative")
    resolved_root = Path(root).resolve()
    resolved = (resolved_root / relative).resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as error:
        raise HarnessError("manifest source path escapes repository root") from error
    return resolved


def load_source_bindings(root, manifest_path, expectations_path, deadline=float("inf")):
    require_deadline(deadline)
    manifest = read_json(manifest_path, "rc manifest")
    expectations = read_json(expectations_path, "import expectations")
    require(type(manifest.get("schema_version")) is int
            and manifest["schema_version"] == 1,
            "rc manifest schema is invalid")
    require(type(expectations.get("schema_version")) is int
            and expectations["schema_version"] == 1,
            "import expectations schema is invalid")

    manifest_hash = sha256_file(Path(manifest_path))
    require_deadline(deadline)
    require(expectations.get("manifest_sha256") == manifest_hash,
            "import expectations do not bind the pinned rc manifest")

    records = manifest.get("records")
    require(isinstance(records, list) and len(records) == 10,
            "rc manifest must contain exactly ten sources")
    bindings = []
    seen = set()
    role_counts = {"container": 0, "item": 0}
    for record in records:
        require(isinstance(record, dict), "rc manifest record must be an object")
        relative_path = record.get("path")
        expected_hash = record.get("sha256")
        expected_size = record.get("bytes")
        role = record.get("role")
        require(type(relative_path) is str and relative_path not in seen,
                "rc manifest paths must be nonempty and unique")
        require(role in role_counts, "rc manifest role is invalid")
        require(type(expected_size) is int and expected_size >= 0,
                "rc manifest byte count is invalid")
        require(type(expected_hash) is str
                and re.fullmatch(r"[0-9a-f]{64}", expected_hash) is not None,
                "rc manifest SHA-256 is invalid")
        source = safe_source_path(root, relative_path)
        require(source.is_file(), f"rc source is missing: {relative_path}")
        require(source.stat().st_size == expected_size,
                f"rc source size differs from manifest: {relative_path}")
        require(sha256_file(source) == expected_hash,
                f"rc source SHA-256 differs from manifest: {relative_path}")
        require_deadline(deadline)
        bindings.append(SourceBinding(relative_path, source, expected_size,
                                      expected_hash))
        seen.add(relative_path)
        role_counts[role] += 1
    require(role_counts == {"container": 6, "item": 4},
            "rc manifest must contain six containers and four items")

    expectation_records = expectations.get("records")
    require(isinstance(expectation_records, list)
            and len(expectation_records) == 10,
            "import expectations must contain exactly ten records")
    expectation_paths = []
    for record in expectation_records:
        require(isinstance(record, dict) and type(record.get("path")) is str,
                "import expectation record is invalid")
        expectation_paths.append(record["path"])
    require(len(set(expectation_paths)) == 10 and set(expectation_paths) == seen,
            "import expectations do not cover exactly the manifest sources")
    return bindings


def finite_vector(value, length, description):
    require(isinstance(value, list) and len(value) == length,
            f"{description} must contain {length} numbers")
    require(all(is_finite_number(component) for component in value),
            f"{description} must contain finite non-boolean numbers")


def close_number(actual, expected):
    return (is_finite_number(actual) and is_finite_number(expected)
            and math.isclose(actual, expected, rel_tol=0.0, abs_tol=1e-8))


def load_expected_matrix(expectations_path):
    document = read_json(expectations_path, "import expectations")
    records = document.get("records")
    require(isinstance(records, list), "import expectations records are invalid")
    bounds = {}
    for record in records:
        require(isinstance(record, dict), "import expectation record is invalid")
        expected = record.get("expected")
        if isinstance(expected, dict) and expected.get("state") == "accepted":
            path = record.get("path")
            mesh_bounds = expected.get("mesh_bounds_mm")
            require(type(path) is str and isinstance(mesh_bounds, dict),
                    "accepted import bounds are missing")
            require_exact_keys(mesh_bounds, {"min", "max"},
                               "accepted mesh bounds")
            finite_vector(mesh_bounds["min"], 3, "mesh minimum")
            finite_vector(mesh_bounds["max"], 3, "mesh maximum")
            bounds[path] = mesh_bounds
    required_paths = set(ITEM_PATHS + CONTAINER_PATHS)
    require(set(bounds) == required_paths,
            "accepted import expectations do not match the nine workloads sources")

    matrix = []
    quaternion = [0.0, 0.0, 0.0, 1.0]
    for object_path in ITEM_PATHS:
        for container_path in CONTAINER_PATHS:
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

            def pose(copy_id, position):
                return {
                    "copy_id": copy_id,
                    "translation_mm": position,
                    "quaternion_xyzw": quaternion.copy(),
                }

            matrix.extend([
                (("centered_single", object_path, container_path),
                 [pose("copy-0", center)], "valid"),
                (("separated_pair", object_path, container_path),
                 [pose("copy-0", first), pose("copy-1", second)], "valid"),
                (("coincident_pair", object_path, container_path),
                 [pose("copy-0", center), pose("copy-1", center.copy())],
                 "invalid"),
                (("outside_min_x", object_path, container_path),
                 [pose("copy-0", outside)], "invalid"),
                (("short_wall_gap", object_path, container_path),
                 [pose("copy-0", short_gap)], "invalid"),
            ])

    analytic_poses = []
    for z_index in range(4):
        for y_index in range(4):
            for x_index in range(4):
                analytic_poses.append({
                    "copy_id": f"copy-{len(analytic_poses)}",
                    "translation_mm": [
                        2.0 + 3.0 * x_index,
                        2.0 + 3.0 * y_index,
                        2.0 + 3.0 * z_index,
                    ],
                    "quaternion_xyzw": quaternion.copy(),
                })
    matrix.append((
        ("analytic_separated_64", "analytic_unit_cube", "analytic_box_14"),
        analytic_poses, "valid"))
    require(len(matrix) == 91, "expected workload matrix is not 91 cases")
    return matrix


def expected_check_states(case, verdict):
    if verdict == "valid":
        return ["complete"] * 6
    if case == "coincident_pair":
        return ["complete", "complete", "not_run", "complete",
                "not_run", "not_run"]
    return ["complete", "complete", "complete", "complete",
            "complete", "not_run"]


def expected_affected_ids(case, verdict):
    if verdict == "valid":
        return set()
    if case == "coincident_pair":
        return {"copy-0", "copy-1"}
    return {"copy-0"}


def verify_pose(actual, expected):
    require_exact_keys(actual,
                       {"copy_id", "translation_mm", "quaternion_xyzw"},
                       "pose")
    require(type(actual["copy_id"]) is str
            and actual["copy_id"] == expected["copy_id"],
            "pose copy ID differs from the fixed matrix")
    for field, length in (("translation_mm", 3), ("quaternion_xyzw", 4)):
        finite_vector(actual[field], length, field)
        require(all(close_number(component, expected_component)
                    for component, expected_component
                    in zip(actual[field], expected[field])),
                f"pose {field} differs from the fixed matrix")


def verify_checks(checks, case, verdict):
    require(isinstance(checks, list) and len(checks) == len(CHECK_NAMES),
            "validation report must contain all six checks")
    expected_states = expected_check_states(case, verdict)
    for index, check in enumerate(checks):
        require_exact_keys(check, {"check", "state", "method"},
                           "validation check")
        require(check["check"] == CHECK_NAMES[index],
                "validation checks are missing, duplicated, or out of order")
        require(check["state"] in {"not_run", "complete", "indeterminate"},
                "validation check state is not a known enum value")
        require(check["state"] == expected_states[index],
                "validation check state does not match the fixed workload")
        require(type(check["method"]) is str and check["method"].strip(),
                "validation check method must be nonempty")


def verify_report(report, case, verdict, poses):
    require_exact_keys(report, {
        "status", "code", "message", "epsilon_mm", "kernel_revision",
        "aabb_pair_tests", "kernel_work", "working_bytes_peak",
        "affected_copy_ids", "affected_ids_truncated", "checks",
    }, "validation report")
    require(report["status"] in {"valid", "invalid"},
            "validation report status must be valid or invalid")
    require(report["status"] == verdict,
            "validation report verdict differs from the fixed workload")
    for field in ("code", "message", "kernel_revision"):
        require(type(report[field]) is str and report[field].strip(),
                f"validation report {field} must be nonempty")
    require(is_finite_number(report["epsilon_mm"])
            and report["epsilon_mm"] > 0,
            "validation epsilon must be finite and positive")
    for field in ("aabb_pair_tests", "kernel_work", "working_bytes_peak"):
        require(is_nonnegative_uint64(report[field]),
                f"validation report {field} must be a nonnegative uint64")
    require(report["aabb_pair_tests"] == len(poses) * (len(poses) - 1) // 2,
            "AABB pair count does not match the complete fixed workload")
    require(report["kernel_work"] > 0 and report["working_bytes_peak"] > 0,
            "validation report must retain actual work and memory counts")
    require(type(report["affected_ids_truncated"]) is bool
            and report["affected_ids_truncated"] is False,
            "fixed workloads must retain complete affected copy IDs")
    affected = report["affected_copy_ids"]
    require(isinstance(affected, list)
            and all(type(copy_id) is str and copy_id for copy_id in affected)
            and len(set(affected)) == len(affected),
            "affected copy IDs must be unique nonempty strings")
    pose_ids = {pose["copy_id"] for pose in poses}
    require(set(affected).issubset(pose_ids),
            "affected copy IDs must reference requested copies")
    require(set(affected) == expected_affected_ids(case, verdict),
            "affected copy IDs do not match the fixed invalid condition")
    verify_checks(report["checks"], case, verdict)


def verify_run(run, run_index, samples, warmup, case, verdict, poses):
    require_exact_keys(run, {
        "phase", "elapsed_ms", "report", "has_validated_solution",
        "validated_copy_count",
    }, "validation run")
    expected_phase = "warmup" if run_index < warmup else "sample"
    require(run["phase"] in {"warmup", "sample"}
            and run["phase"] == expected_phase,
            "validation run phases or warmup count are invalid")
    require(is_finite_number(run["elapsed_ms"])
            and run["elapsed_ms"] >= 0,
            "validation elapsed time must be finite and nonnegative")
    require(type(run["has_validated_solution"]) is bool,
            "snapshot-presence flag must be boolean")
    require(type(run["validated_copy_count"]) is int
            and run["validated_copy_count"] >= 0,
            "validated copy count must be a nonnegative integer")
    verify_report(run["report"], case, verdict, poses)
    if verdict == "valid":
        require(run["has_validated_solution"]
                and run["validated_copy_count"] == len(poses),
                "valid workload must retain the complete validated snapshot")
    else:
        require(not run["has_validated_solution"]
                and run["validated_copy_count"] == 0,
                "invalid workload must not publish a validated snapshot")


def verify_payload(payload, expectations_path, samples, warmup):
    require(type(samples) is int and 3 <= samples <= 20,
            "qualification requires 3-20 samples")
    require(type(warmup) is int and 1 <= warmup <= 10,
            "qualification requires 1-10 warmups")
    require_exact_keys(payload, {
        "schema_version", "benchmark_kind", "compiler", "build_type",
        "parameters", "setup_ms", "workloads",
    }, "native validation payload")
    require(type(payload["schema_version"]) is int
            and payload["schema_version"] == 1,
            "native payload schema version is invalid")
    require(payload["benchmark_kind"] == "native_validation",
            "native payload benchmark kind is invalid")
    require(type(payload["compiler"]) is str and payload["compiler"].strip(),
            "native payload compiler is missing")
    require(payload["build_type"] == "Release",
            "native payload must come from a Release build")
    require_exact_keys(payload["parameters"], {"samples", "warmup"},
                       "native parameters")
    parameters = payload["parameters"]
    require(type(parameters["samples"]) is int
            and parameters["samples"] == samples,
            "native sample count differs from the request")
    require(type(parameters["warmup"]) is int
            and parameters["warmup"] == warmup,
            "native warmup count differs from the request")
    require(is_finite_number(payload["setup_ms"]) and payload["setup_ms"] >= 0,
            "native setup time must be finite and nonnegative")

    expected_matrix = load_expected_matrix(expectations_path)
    workloads = payload["workloads"]
    require(isinstance(workloads, list) and len(workloads) == 91,
            "native payload must contain exactly 91 workloads")
    verified = set()
    for index, (workload, expected) in enumerate(zip(workloads, expected_matrix)):
        require_exact_keys(workload, {
            "case", "object_path", "container_path", "poses",
            "constraints", "runs",
        }, "validation workload")
        key = (workload["case"], workload["object_path"],
               workload["container_path"])
        expected_key, expected_poses, verdict = expected
        require(key == expected_key and key not in verified,
                f"workload {index} is forged, duplicated, or out of order")
        verified.add(key)

        constraints = workload["constraints"]
        require_exact_keys(constraints,
                           {"pair_clearance_mm", "wall_clearance_mm"},
                           "validation constraints")
        for field in ("pair_clearance_mm", "wall_clearance_mm"):
            require(is_finite_number(constraints[field])
                    and constraints[field] == 1.0,
                    f"{field} must be exactly 1 mm")

        poses = workload["poses"]
        require(isinstance(poses, list) and len(poses) == len(expected_poses),
                "workload pose count differs from the fixed matrix")
        for actual_pose, expected_pose in zip(poses, expected_poses):
            verify_pose(actual_pose, expected_pose)

        runs = workload["runs"]
        require(isinstance(runs, list) and len(runs) == warmup + samples,
                "workload run count differs from requested warmups and samples")
        deterministic_report = None
        for run_index, run in enumerate(runs):
            verify_run(run, run_index, samples, warmup, workload["case"],
                       verdict, poses)
            report_bytes = json.dumps(
                run["report"], sort_keys=True, separators=(",", ":"),
                allow_nan=False).encode("utf-8")
            if deterministic_report is None:
                deterministic_report = report_bytes
            else:
                require(report_bytes == deterministic_report,
                        "validation report or work counts changed between runs")
    require(len(verified) == 91, "native payload workload matrix is incomplete")
    return verified


def summarize_performance(payload):
    performance = []
    for workload in payload["workloads"]:
        samples = [run["elapsed_ms"] for run in workload["runs"]
                   if run["phase"] == "sample"]
        report = workload["runs"][0]["report"]
        performance.append({
            "case": workload["case"],
            "object_path": workload["object_path"],
            "container_path": workload["container_path"],
            "samples_ms": samples,
            **statistics_for(samples),
            "work_counts": {
                "aabb_pair_tests": report["aabb_pair_tests"],
                "kernel_work": report["kernel_work"],
                "working_bytes_peak": report["working_bytes_peak"],
            },
        })
    return performance


def capture_identities(paths, deadline):
    identities = {}
    for path in paths:
        require_deadline(deadline)
        resolved = Path(path).resolve()
        require(resolved.is_file(), f"protected input is missing: {resolved}")
        stat = resolved.stat()
        digest = sha256_file(resolved)
        require_deadline(deadline)
        identities[str(resolved)] = {
            "device": stat.st_dev,
            "inode": stat.st_ino,
            "bytes": stat.st_size,
            "modified_ns": stat.st_mtime_ns,
            "sha256": digest,
        }
    return identities


def require_bindings_match_identities(bindings, identities):
    for binding in bindings:
        identity = identities.get(str(binding.path.resolve()))
        require(identity is not None,
                f"source is missing from protected baseline: {binding.relative_path}")
        require(identity["bytes"] == binding.size
                and identity["sha256"] == binding.sha256,
                f"source changed after manifest validation: {binding.relative_path}")


def raw_stream(value):
    if value is None:
        return b""
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("utf-8", errors="surrogatepass")
    return bytes(value)


def write_process_artifacts(artifact_directory, stdout, stderr):
    Path(artifact_directory, "stdout.txt").write_bytes(raw_stream(stdout))
    Path(artifact_directory, "stderr.txt").write_bytes(raw_stream(stderr))


def run_native(command, timeout_seconds, deadline, artifact_directory):
    require_deadline(deadline)
    remaining = deadline - time.monotonic()
    require(remaining > 0, "overall validation qualification deadline expired")
    timeout = min(timeout_seconds, remaining)
    try:
        completed = subprocess.run(
            command, capture_output=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        write_process_artifacts(
            artifact_directory,
            error.stdout if error.stdout is not None else error.output,
            error.stderr)
        raise HarnessError(
            f"validation benchmark watchdog expired after {timeout:g} seconds") from error
    except OSError as error:
        raise HarnessError(f"validation benchmark process failed: {error}") from error
    write_process_artifacts(artifact_directory, completed.stdout, completed.stderr)
    require(completed.returncode == 0,
            f"native validation benchmark failed with exit {completed.returncode}")
    require_deadline(deadline)
    try:
        text = raw_stream(completed.stdout).decode("utf-8", errors="strict")
        payload = json.loads(text)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"native validation stdout is not UTF-8 JSON: {error}") from error
    require(isinstance(payload, dict), "native validation payload must be an object")
    return payload


def repository_metadata(root, deadline):
    require_deadline(deadline)
    timeout = max(0.001, min(5.0, deadline - time.monotonic()))
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=root, capture_output=True,
            timeout=timeout, check=False)
        require_deadline(deadline)
        timeout = max(0.001, min(5.0, deadline - time.monotonic()))
        status = subprocess.run(
            ["git", "status", "--porcelain"], cwd=root, capture_output=True,
            timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise HarnessError(f"cannot record repository provenance: {error}") from error
    require(revision.returncode == 0 and status.returncode == 0,
            "cannot record repository provenance")
    try:
        revision_text = revision.stdout.decode("ascii", errors="strict").strip()
    except UnicodeError as error:
        raise HarnessError("repository revision is not ASCII") from error
    require(re.fullmatch(r"[0-9a-fA-F]{40,64}", revision_text) is not None,
            "repository revision is malformed")
    require_deadline(deadline)
    return {"revision": revision_text.lower(), "dirty": bool(status.stdout.strip())}


def provenance_hashes(identities, executable, metadata_path,
                      manifest_path, expectations_path, bindings):
    def evidence(path):
        identity = identities[str(Path(path).resolve())]
        return {"sha256": identity["sha256"], "bytes": identity["bytes"]}

    return {
        "executable": evidence(executable),
        "build_metadata": evidence(metadata_path),
        "manifest": evidence(manifest_path),
        "expectations": evidence(expectations_path),
        "sources": {
            binding.relative_path: evidence(binding.path) for binding in bindings
        },
    }


def publish_report(output, report, deadline, protected_paths, baseline_identities):
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f".{uuid.uuid4().hex}.tmp")
    try:
        require_deadline(deadline)
        with temporary.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        require(capture_identities(protected_paths, deadline) == baseline_identities,
                "protected executable, metadata, fixture, or oracle changed")
        ensure_distinct_output(output, protected_paths)
        require_deadline(deadline)
        os.replace(temporary, output)
    except Exception:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def parse_arguments(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--build-metadata", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=float, default=300)
    parser.add_argument("--overall-timeout-seconds", type=float, default=600)
    return parser.parse_args(argv)


def validate_arguments(args):
    require(type(args.samples) is int and 3 <= args.samples <= 20,
            "samples must be an integer from 3 through 20")
    require(type(args.warmup) is int and 1 <= args.warmup <= 10,
            "warmup must be an integer from 1 through 10")
    require(is_finite_number(args.timeout_seconds) and args.timeout_seconds > 0,
            "process timeout must be finite and positive")
    require(is_finite_number(args.overall_timeout_seconds)
            and args.overall_timeout_seconds > 0,
            "overall timeout must be finite and positive")


def main(argv=None):
    args = parse_arguments(argv)
    started = time.monotonic()
    deadline = started + args.overall_timeout_seconds
    try:
        validate_arguments(args)
        executable = Path(args.executable).resolve()
        metadata_path = Path(args.build_metadata).resolve()
        output = Path(args.output).resolve()
        manifest_path = Path(MANIFEST).resolve()
        expectations_path = Path(EXPECT).resolve()
        require(executable.is_file(), "validation benchmark executable is missing")
        require(metadata_path.is_file(), "build metadata is missing")

        bindings = load_source_bindings(
            ROOT, manifest_path, expectations_path, deadline)
        protected_paths = [
            executable, metadata_path, manifest_path, expectations_path,
            *(binding.path for binding in bindings),
        ]
        ensure_distinct_output(output, protected_paths)
        build_metadata = read_json(metadata_path, "build metadata")
        require(build_metadata.get("build_type") == "Release",
                "only Release build metadata can qualify validation")
        baseline_identities = capture_identities(protected_paths, deadline)
        require_bindings_match_identities(bindings, baseline_identities)

        artifact_directory = output.parent / (
            output.name + f".artifacts-{uuid.uuid4().hex}")
        artifact_directory.mkdir(parents=True, exist_ok=False)
        command = [
            str(executable), "--repo-root", str(ROOT),
            "--samples", str(args.samples),
            "--warmup", str(args.warmup),
        ]
        payload = run_native(
            command, args.timeout_seconds, deadline, artifact_directory)
        verify_payload(payload, expectations_path, args.samples, args.warmup)
        require(payload["build_type"] == build_metadata["build_type"],
                "native build type differs from build metadata")
        require(capture_identities(protected_paths, deadline) == baseline_identities,
                "protected executable, metadata, fixture, or oracle changed")

        repository = repository_metadata(ROOT, deadline)
        performance = summarize_performance(payload)
        require_deadline(deadline)
        report = {
            "schema_version": 1,
            "benchmark_kind": "native_validation_qualification",
            "qualified": True,
            "metadata": {
                "build": build_metadata,
                "host": {
                    "platform": platform.platform(),
                    "cpu": platform.processor(),
                    "python": platform.python_version(),
                },
                "repository": repository,
                "parameters": {
                    "samples": args.samples,
                    "warmup": args.warmup,
                    "timeout_seconds": args.timeout_seconds,
                    "overall_timeout_seconds": args.overall_timeout_seconds,
                },
                "artifact_directory": str(artifact_directory),
            },
            "hashes": provenance_hashes(
                baseline_identities, executable, metadata_path,
                manifest_path, expectations_path, bindings),
            "performance": performance,
            "payload": payload,
        }
        publish_report(output, report, deadline, protected_paths,
                       baseline_identities)
        print(f"validation qualification report: {output}")
        return 0
    except (HarnessError, OSError, UnicodeError, json.JSONDecodeError,
            TypeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
