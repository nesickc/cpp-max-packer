"""Run the bounded native reference oracle and retain comparable timing reports."""

import argparse
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys
from pathlib import Path


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    return 2


def require(condition, message):
    if not condition:
        raise ValueError(message)


def is_integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


def is_finite_number(value):
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return -sys.float_info.max <= value <= sys.float_info.max
    if isinstance(value, float):
        return math.isfinite(value)
    return False


def validate_native_payload(raw, expected_samples=None):
    require(isinstance(raw, dict), "native payload must be an object")
    require(raw.get("schema_version") == 1 and is_integer(raw.get("schema_version")), "invalid native schema version")
    require(raw.get("benchmark_kind") == "test_reference_oracle", "invalid native benchmark kind")
    for field in ("compiler", "build_type"):
        require(isinstance(raw.get(field), str) and raw[field].strip(), f"invalid native {field}")
    workloads = raw.get("workloads")
    require(isinstance(workloads, list) and workloads, "native workloads must be a nonempty list")
    names = set()
    for workload in workloads:
        require(isinstance(workload, dict), "native workload must be an object")
        name = workload.get("name")
        require(isinstance(name, str) and name and name not in names, "invalid or duplicate workload name")
        names.add(name)
        require(is_integer(workload.get("work_units")) and workload["work_units"] > 0, "invalid workload work_units")
        require(isinstance(workload.get("checksum"), str) and workload["checksum"], "invalid workload checksum")
        samples = workload.get("samples_ms")
        require(isinstance(samples, list) and samples, "invalid workload samples")
        require(all(is_finite_number(sample) and sample >= 0 for sample in samples), "invalid workload sample")
        if expected_samples is not None:
            require(len(samples) == expected_samples, "native sample count mismatch")
    return raw


def statistics_for(samples):
    ordered = sorted(samples)
    return {"median_ms": statistics.median(samples), "min_ms": ordered[0],
            "p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1]}


def repository_metadata():
    try:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=Path(__file__).parents[1], text=True, stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=Path(__file__).parents[1], text=True, stderr=subprocess.DEVNULL).strip())
        return {"git_revision": revision, "git_dirty": dirty}
    except (OSError, subprocess.CalledProcessError):
        return {"git_revision": None, "git_dirty": None}


def build_report(raw, samples, warmup, executable_sha256):
    workloads = []
    for workload in raw["workloads"]:
        recorded = dict(workload)
        recorded.update(statistics_for(recorded["samples_ms"]))
        workloads.append(recorded)
    metadata = {
        "python": platform.python_version(), "os": platform.platform(),
        "cpu": platform.processor(), "architecture": platform.machine(),
        "native": {"compiler": raw["compiler"], "build_type": raw["build_type"]},
        "parameters": {"samples": samples, "warmup": warmup},
        "executable_sha256": executable_sha256,
    }
    metadata.update(repository_metadata())
    return {"schema_version": 1, "benchmark_kind": "test_reference_oracle", "metadata": metadata, "workloads": workloads}


def validate_baseline_report(raw):
    require(isinstance(raw, dict), "baseline must be an object")
    require(raw.get("schema_version") == 1 and is_integer(raw.get("schema_version")), "invalid baseline schema version")
    require(raw.get("benchmark_kind") == "test_reference_oracle", "invalid baseline benchmark kind")
    metadata = raw.get("metadata")
    require(isinstance(metadata, dict), "baseline metadata must be an object")
    native = metadata.get("native")
    parameters = metadata.get("parameters")
    require(isinstance(native, dict) and isinstance(parameters, dict), "invalid baseline metadata")
    native_payload = {"schema_version": raw["schema_version"], "benchmark_kind": raw["benchmark_kind"],
                      "compiler": native.get("compiler"), "build_type": native.get("build_type"),
                      "workloads": raw.get("workloads")}
    validate_native_payload(native_payload)
    require(is_integer(parameters.get("samples")) and parameters["samples"] > 0, "invalid baseline sample parameter")
    require(is_integer(parameters.get("warmup")) and parameters["warmup"] >= 0, "invalid baseline warmup parameter")
    require(all(isinstance(metadata.get(field), str) for field in ("os", "cpu", "architecture")), "invalid baseline host metadata")
    for workload in native_payload["workloads"]:
        require(len(workload["samples_ms"]) == parameters["samples"], "baseline sample count mismatch")
    return native_payload, metadata


def compare_reports(report, baseline):
    baseline_payload, baseline_metadata = validate_baseline_report(baseline)
    metadata = report["metadata"]
    require(metadata["native"]["build_type"] == "Release", "only Release runs may compare a baseline")
    require(baseline_metadata["native"].get("build_type") == "Release", "baseline must be a Release report")
    require(metadata["native"] == baseline_metadata["native"], "incompatible baseline native build")
    require(metadata["parameters"] == baseline_metadata["parameters"], "incompatible baseline parameters")
    require(all(metadata[field] == baseline_metadata[field] for field in ("os", "cpu", "architecture")), "incompatible baseline host")
    current = {(workload["name"], workload["work_units"], workload["checksum"]): workload for workload in report["workloads"]}
    previous = {(workload["name"], workload["work_units"], workload["checksum"]): workload for workload in baseline_payload["workloads"]}
    require(set(current) == set(previous), "incompatible baseline workloads")
    for identity, workload in current.items():
        baseline_median = statistics_for(previous[identity]["samples_ms"])["median_ms"]
        limit = baseline_median * (1 + report["max_regression_percent"] / 100)
        require(workload["median_ms"] <= limit, "performance regression")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=float, default=60)
    parser.add_argument("--baseline")
    parser.add_argument("--max-regression-percent", type=float, default=20)
    args = parser.parse_args()
    try:
        require(args.samples > 0 and args.warmup >= 0, "invalid samples or warmup")
        require(math.isfinite(args.timeout_seconds) and args.timeout_seconds > 0, "invalid timeout")
        require(math.isfinite(args.max_regression_percent) and args.max_regression_percent >= 0, "invalid regression tolerance")
        output = Path(args.output).resolve()
        baseline_path = Path(args.baseline).resolve() if args.baseline else None
        require(baseline_path != output, "baseline and output must be different paths")
        executable = Path(args.executable).resolve()
        command = [str(executable), "--samples", str(args.samples), "--warmup", str(args.warmup)]
        executable_sha256 = hashlib.sha256(executable.read_bytes()).hexdigest()
        if executable.suffix.lower() == ".py":
            command.insert(0, sys.executable)
        completed = subprocess.run(command, cwd=Path(__file__).parents[1], capture_output=True, text=True, timeout=args.timeout_seconds)
        require(completed.returncode == 0, "native executable failed")
        raw = validate_native_payload(json.loads(completed.stdout), args.samples)
        report = build_report(raw, args.samples, args.warmup, executable_sha256)
        report["max_regression_percent"] = args.max_regression_percent
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
        if baseline_path:
            compare_reports(report, json.loads(baseline_path.read_text(encoding="utf-8")))
        print(f"reference oracle report: {output}")
        return 0
    except (OSError, ValueError, json.JSONDecodeError, subprocess.TimeoutExpired) as error:
        return fail(str(error))


if __name__ == "__main__":
    sys.exit(main())
