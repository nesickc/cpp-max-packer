"""Strict publisher for the T-006 physical AABB baseline qualification."""

import argparse
import json
import math
import platform
import re
import sys
import time
import uuid
from pathlib import Path

try:
    from benchmarks import baseline_checks
    from benchmarks.run_import import statistics_for
    from benchmarks.run_validation import (
        HarnessError, capture_identities, ensure_distinct_output, load_source_bindings,
        provenance_hashes, publish_report, repository_metadata, require,
        require_bindings_match_identities, require_deadline, run_native)
except ModuleNotFoundError:
    import baseline_checks
    from run_import import statistics_for
    from run_validation import (
        HarnessError, capture_identities, ensure_distinct_output, load_source_bindings,
        provenance_hashes, publish_report, repository_metadata, require,
        require_bindings_match_identities, require_deadline, run_native)


ROOT = Path(__file__).parents[1]
MANIFEST = ROOT / "tests" / "fixtures" / "rc-manifest.json"
EXPECTATIONS = ROOT / "tests" / "fixtures" / "import-expectations.json"


def bounded_int(text, low, high):
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("integer required") from error
    if value < low or value > high:
        raise argparse.ArgumentTypeError(f"must be {low}..{high}")
    return value


def positive_number(text):
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("number required") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("positive finite number required")
    return value


def parse_arguments(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--build-metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=lambda value: bounded_int(value, 3, 20), default=3)
    parser.add_argument("--warmup", type=lambda value: bounded_int(value, 1, 10), default=1)
    parser.add_argument("--timeout-seconds", type=positive_number, default=600)
    parser.add_argument("--overall-timeout-seconds", type=positive_number, default=900)
    parser.add_argument("--pilot", action="store_true")
    return parser.parse_args(argv)


def build_native_command(executable, arguments):
    return [str(executable), *arguments]


def harness_paths(root):
    directory = Path(root).resolve() / "benchmarks"
    return [directory / "run_baseline.py", directory / "baseline_checks.py",
            directory / "run_validation.py", directory / "representation_checks.py",
            directory / "run_import.py", directory / "run_representation.py"]


def read_bound_json(path, description, identities):
    path = Path(path).resolve()
    identity = identities.get(str(path))
    require(identity is not None, f"{description} is not protected")
    raw = path.read_bytes()
    require(len(raw) == identity["bytes"], f"{description} changed before parsing")
    try:
        value = baseline_checks.strict_json(raw.decode("utf-8", errors="strict"))
    except (UnicodeError, ValueError) as error:
        raise HarnessError(f"cannot read {description}: {error}") from error
    return value


def normalized_msvc_version(value):
    require(type(value) is str, "build metadata compiler is missing")
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\.\d+)?", value)
    require(match is not None, "build metadata compiler is malformed")
    major, minor, build = (int(match.group(index)) for index in range(1, 4))
    require(major == 19 and 0 <= minor <= 99 and 0 <= build <= 99999,
            "build metadata compiler is not MSVC")
    return int(f"{major:02d}{minor:02d}{build:05d}")


def require_build_metadata(metadata, repository):
    require(isinstance(repository, dict) and
            repository.get("dirty") is False,
            "baseline qualification requires a clean repository")
    require(metadata.get("build_type") == "Release", "only Release metadata can qualify")
    compiler = normalized_msvc_version(metadata.get("compiler"))
    require(metadata.get("compiler") == metadata.get("compiler_file_version"),
            "build metadata compiler identities disagree")
    revision = metadata.get("source_revision")
    require(type(revision) is str and re.fullmatch(r"[0-9a-fA-F]{40,64}", revision),
            "build source revision is malformed")
    require(revision.lower() == repository["revision"], "build source revision differs")
    return compiler


def reread_native_stdout(artifact_directory, weak_payload):
    path = Path(artifact_directory) / "stdout.txt"
    try:
        payload = baseline_checks.strict_json(path.read_bytes().decode("utf-8", errors="strict"))
    except (OSError, UnicodeError, ValueError) as error:
        raise HarnessError(f"cannot re-read native stdout: {error}") from error
    require(payload == weak_payload, "retained native stdout differs from process payload")
    return payload


def extended_hashes(identities, executable, metadata, bindings, harness):
    result = provenance_hashes(identities, executable, metadata, MANIFEST, EXPECTATIONS, bindings)
    result["harness"] = {Path(path).name: identities[str(Path(path).resolve())]
                         for path in harness}
    return result


def timing_summary(payload):
    workloads = []
    for workload in payload["workloads"]:
        measured = [run for run in workload["runs"]
                    if run["phase"] == "sample"]
        search = [run["search_ms"] for run in measured]
        fresh = [run["fresh_revalidation_ms"] for run in measured]
        workloads.append({
            "id": workload["id"],
            "search": {"samples_ms": search, **statistics_for(search)},
            "fresh_revalidation": {
                "samples_ms": fresh, **statistics_for(fresh),
            },
        })
    return {
        "setup_ms": payload["setup_ms"],
        "process_duration_ms": payload["process_duration_ms"],
        "workloads": workloads,
    }


def main(argv=None):
    args = parse_arguments(argv)
    deadline = time.monotonic() + args.overall_timeout_seconds
    try:
        executable, metadata_path, output = (args.executable.resolve(),
                                             args.build_metadata.resolve(), args.output.resolve())
        manifest, expectations = MANIFEST.resolve(), EXPECTATIONS.resolve()
        require(executable.is_file(), "baseline executable is missing")
        require(metadata_path.is_file(), "build metadata is missing")
        bindings = load_source_bindings(ROOT, manifest, expectations, deadline)
        harness = harness_paths(ROOT)
        require(all(path.is_file() for path in harness), "baseline harness source is missing")
        protected = [*(binding.path for binding in bindings), manifest, expectations,
                     executable, metadata_path, *harness]
        ensure_distinct_output(output, protected)
        baseline = capture_identities(protected, deadline)
        require_bindings_match_identities(bindings, baseline)
        metadata = read_bound_json(metadata_path, "build metadata", baseline)
        repository = repository_metadata(ROOT, deadline)
        compiler = require_build_metadata(metadata, repository)
        artifact_directory = output.parent / (output.name + f".artifacts-{uuid.uuid4().hex}")
        artifact_directory.parent.mkdir(parents=True, exist_ok=True)
        artifact_directory.mkdir(exist_ok=False)
        native_arguments = ["--repo-root", str(ROOT.resolve()), "--samples", str(args.samples),
                            "--warmup", str(args.warmup)]
        if args.pilot:
            native_arguments.append("--pilot")
        weak_payload = run_native(build_native_command(executable, native_arguments),
                                  args.timeout_seconds, deadline, artifact_directory)
        payload = reread_native_stdout(artifact_directory, weak_payload)
        baseline_checks.verify_payload(payload, expectations, args.samples, args.warmup)
        build = payload.get("build", {})
        require(build.get("compiler_version") == compiler, "native compiler differs from metadata")
        require(capture_identities(protected, deadline) == baseline,
                "protected source, harness, executable, metadata, or oracle changed")
        require_bindings_match_identities(bindings, baseline)
        require(repository_metadata(ROOT, deadline) == repository,
                "repository identity changed during qualification")
        require_deadline(deadline)
        report = {"schema_version": 1,
                  "benchmark_kind": "native_physical_aabb_baseline_qualification",
                  "qualified": True,
                  "metadata": {"build": metadata, "repository": repository,
                               "host": {"platform": platform.platform(),
                                        "cpu": platform.processor(),
                                        "python": platform.python_version()},
                               "parameters": {"samples": args.samples, "warmup": args.warmup,
                                              "timeout_seconds": args.timeout_seconds,
                                              "overall_timeout_seconds":
                                                  args.overall_timeout_seconds},
                               "artifact_directory": str(artifact_directory)},
                  "hashes": extended_hashes(baseline, executable, metadata_path, bindings, harness),
                  "performance": timing_summary(payload),
                  "payload": payload}
        publish_report(output, report, deadline, protected, baseline)
        print(f"baseline qualification report: {output}")
        return 0
    except (HarnessError, OSError, UnicodeError, ValueError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
