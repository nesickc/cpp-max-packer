"""Strict publisher for the native geometry-representation qualification."""

import argparse
import ctypes
import hashlib
import json
import math
import platform
import re
import struct
import sys
import time
import uuid
from pathlib import Path

try:
    from benchmarks.run_validation import (
        HarnessError, capture_identities, ensure_distinct_output,
        load_source_bindings, provenance_hashes, publish_report,
        repository_metadata, require, require_bindings_match_identities,
        require_deadline, run_native,
    )
except ModuleNotFoundError:  # Direct execution places benchmarks/ on sys.path.
    from run_validation import (
        HarnessError, capture_identities, ensure_distinct_output,
        load_source_bindings, provenance_hashes, publish_report,
        repository_metadata, require, require_bindings_match_identities,
        require_deadline, run_native,
    )


ROOT = Path(__file__).parents[1]
MANIFEST = ROOT / "tests" / "fixtures" / "rc-manifest.json"
EXPECTATIONS = ROOT / "tests" / "fixtures" / "import-expectations.json"
LARGE_SHA256 = "dedcf5d412dd6ab34ba88cd5baeafcf50467f18b5bb1567614051e3f337047f9"
LARGE_BYTES = 54_000_084
LARGE_TRIANGLES = 1_080_000
LARGE_SUBDIVISIONS = 300
REPRESENTATION_CAP_BYTES = 2 * 1024**3
LARGE_HEADER = b"SpectraPack subdivided cube n=300".ljust(80, b" ")


class MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.c_ulong),
        ("dwMemoryLoad", ctypes.c_ulong),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def _payload_checks():
    try:
        from benchmarks import representation_checks
    except ModuleNotFoundError:  # Direct execution places benchmarks/ on sys.path.
        import representation_checks
    return representation_checks


def strict_json(text):
    return _payload_checks().strict_json(text)


def verify_payload(payload, expectations_path, samples, warmup):
    return _payload_checks().verify_payload(
        payload, expectations_path, samples, warmup)


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
    parser.add_argument("--large-fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=lambda value: bounded_int(value, 3, 20),
                        default=3)
    parser.add_argument("--warmup", type=lambda value: bounded_int(value, 1, 10),
                        default=1)
    parser.add_argument("--timeout-seconds", type=positive_number, default=900)
    parser.add_argument("--overall-timeout-seconds", type=positive_number,
                        default=1200)
    return parser.parse_args(argv)


def host_memory_status():
    require(sys.platform == "win32",
            "representation qualification requires Windows host memory data")
    status = MemoryStatusEx()
    status.dwLength = ctypes.sizeof(status)
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        function = kernel32.GlobalMemoryStatusEx
        function.argtypes = [ctypes.POINTER(MemoryStatusEx)]
        function.restype = ctypes.c_int
        succeeded = function(ctypes.byref(status))
    except (AttributeError, OSError) as error:
        raise HarnessError(f"cannot measure host physical memory: {error}") from error
    if not succeeded:
        raise HarnessError(
            f"cannot measure host physical memory: Windows error "
            f"{ctypes.get_last_error()}")
    result = {
        "total_physical_bytes": int(status.ullTotalPhys),
        "available_physical_bytes": int(status.ullAvailPhys),
    }
    require(result["total_physical_bytes"] > 0
            and 0 < result["available_physical_bytes"]
            <= result["total_physical_bytes"],
            "host physical memory measurement is invalid")
    return result


def require_memory_admission(memory):
    total = memory.get("total_physical_bytes")
    available = memory.get("available_physical_bytes")
    require(type(total) is int and type(available) is int
            and total > 0 and 0 < available <= total,
            "host physical memory measurement is invalid")
    require(REPRESENTATION_CAP_BYTES * 2 <= available,
            "2 GiB representation allowance exceeds half available physical memory")


def require_large_fixture(path, identities):
    identity = identities.get(str(path.resolve()))
    require(identity is not None, "large generated fixture is not protected")
    require(identity["bytes"] == LARGE_BYTES,
            "large generated fixture byte count differs")
    require(identity["sha256"] == LARGE_SHA256,
            "large generated fixture SHA-256 differs")
    with path.open("rb") as stream:
        header = stream.read(84)
    require(len(header) == 84 and header[:80] == LARGE_HEADER,
            "large generated fixture header or n=300 identity differs")
    require(struct.unpack("<I", header[80:84])[0] == LARGE_TRIANGLES,
            "large generated fixture triangle header differs")


def read_bound_json(path, description, identities, deadline):
    require_deadline(deadline)
    identity = identities.get(str(path.resolve()))
    require(identity is not None, f"{description} is not protected")
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(text)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"cannot read {description}: {error}") from error
    require(len(raw) == identity["bytes"]
            and hashlib.sha256(raw).hexdigest() == identity["sha256"],
            f"{description} bytes differ from protected baseline")
    require(isinstance(value, dict), f"{description} must be a JSON object")
    require_deadline(deadline)
    return value


def harness_paths(root):
    benchmark_directory = Path(root).resolve() / "benchmarks"
    return [
        Path(__file__).resolve(),
        benchmark_directory / "representation_checks.py",
        benchmark_directory / "run_validation.py",
        benchmark_directory / "run_import.py",
    ]


def normalized_msvc_version(value, description):
    require(type(value) is str, f"{description} is missing")
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?", value)
    require(match is not None, f"{description} is malformed")
    major, minor, build = (int(match.group(index)) for index in range(1, 4))
    require(major == 19 and 0 <= minor <= 99 and 0 <= build <= 99999,
            f"{description} is not an MSVC compiler version")
    return int(f"{major:02d}{minor:02d}{build:05d}")


def require_build_metadata(metadata, repository):
    require(metadata.get("build_type") == "Release",
            "only Release build metadata can qualify representations")
    compiler = normalized_msvc_version(
        metadata.get("compiler"), "build metadata compiler")
    compiler_file = normalized_msvc_version(
        metadata.get("compiler_file_version"),
        "build metadata compiler file version")
    require(metadata["compiler"] == metadata["compiler_file_version"]
            and compiler == compiler_file,
            "build metadata compiler identities disagree")
    source_revision = metadata.get("source_revision")
    require(type(source_revision) is str
            and re.fullmatch(r"[0-9a-fA-F]{40,64}", source_revision) is not None,
            "build source revision is missing or malformed")
    require(source_revision.lower() == repository["revision"],
            "build source revision differs from repository revision")
    return compiler


def require_native_build(payload, metadata_compiler):
    build = payload.get("build")
    require(isinstance(build, dict), "native build identity is missing")
    require(build.get("build_type") == "Release",
            "native result is not Release")
    require(build.get("compiler") == "msvc",
            "native result compiler is not MSVC")
    version = build.get("compiler_version")
    require(type(version) is int and version == metadata_compiler,
            "native MSVC version differs from build metadata")


def process_peak_working_set(payload):
    before = payload.get("process_peak_working_set_bytes_before")
    after = payload.get("process_peak_working_set_bytes_after")
    require(type(before) is int and type(after) is int
            and 0 <= before <= after,
            "native process peak working set measurement is invalid")
    return {"before": before, "after": after}


def reread_native_stdout(artifact_directory, weak_payload):
    stdout = Path(artifact_directory) / "stdout.txt"
    try:
        text = stdout.read_bytes().decode("utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        raise HarnessError(f"cannot re-read retained native stdout: {error}") from error
    payload = strict_json(text)
    require(payload == weak_payload,
            "retained native stdout differs from parsed process result")
    return payload


def extended_hashes(identities, executable, metadata_path, manifest_path,
                    expectations_path, bindings, generator, large_fixture,
                    harness):
    result = provenance_hashes(
        identities, executable, metadata_path, manifest_path,
        expectations_path, bindings)

    def evidence(path):
        identity = identities[str(Path(path).resolve())]
        return {"sha256": identity["sha256"], "bytes": identity["bytes"]}

    result["generator"] = evidence(generator)
    result["large_fixture"] = evidence(large_fixture)
    result["harness"] = {path.name: evidence(path) for path in harness}
    return result


def main(argv=None):
    args = parse_arguments(argv)
    deadline = time.monotonic() + args.overall_timeout_seconds
    try:
        executable = args.executable.resolve()
        metadata_path = args.build_metadata.resolve()
        large_fixture = args.large_fixture.resolve()
        output = args.output.resolve()
        manifest_path = MANIFEST.resolve()
        expectations_path = EXPECTATIONS.resolve()
        generator = (ROOT / "benchmarks" / "subdivided_cube.py").resolve()

        require(executable.is_file(),
                "representation benchmark executable is missing")
        require(metadata_path.is_file(), "build metadata is missing")
        require(generator.is_file(), "large fixture generator is missing")
        bindings = load_source_bindings(
            ROOT, manifest_path, expectations_path, deadline)
        harness = harness_paths(ROOT)
        protected = [
            *(binding.path for binding in bindings), manifest_path,
            expectations_path, generator, large_fixture, executable,
            metadata_path, *harness,
        ]
        ensure_distinct_output(output, protected)
        baseline = capture_identities(protected, deadline)
        require_bindings_match_identities(bindings, baseline)
        require_large_fixture(large_fixture, baseline)
        metadata = read_bound_json(
            metadata_path, "build metadata", baseline, deadline)
        repository = repository_metadata(ROOT, deadline)
        metadata_compiler = require_build_metadata(metadata, repository)
        memory = host_memory_status()
        require_memory_admission(memory)

        output.parent.mkdir(parents=True, exist_ok=True)
        artifact_directory = output.parent / (
            output.name + f".artifacts-{uuid.uuid4().hex}")
        artifact_directory.mkdir(parents=True, exist_ok=False)
        command = [
            str(executable), "--repo-root", str(ROOT.resolve()),
            "--large-fixture", str(large_fixture),
            "--samples", str(args.samples),
            "--warmup", str(args.warmup),
        ]
        weak_payload = run_native(
            command, args.timeout_seconds, deadline, artifact_directory)
        payload = reread_native_stdout(artifact_directory, weak_payload)
        performance = verify_payload(
            payload, expectations_path, args.samples, args.warmup)
        require_native_build(payload, metadata_compiler)
        process_memory = process_peak_working_set(payload)

        final_identities = capture_identities(protected, deadline)
        require(final_identities == baseline,
                "protected source, harness, executable, metadata, or oracle changed")
        require_bindings_match_identities(bindings, final_identities)
        require(repository_metadata(ROOT, deadline) == repository,
                "repository revision or dirty state changed during qualification")
        require_deadline(deadline)

        report = {
            "schema_version": 1,
            "benchmark_kind": "native_representation_qualification",
            "qualified": True,
            "metadata": {
                "build": metadata,
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
                    "generator": {"subdivisions": LARGE_SUBDIVISIONS},
                    "large_representation_cap_bytes": REPRESENTATION_CAP_BYTES,
                },
                "artifact_directory": str(artifact_directory),
            },
            "resources": {
                "host_physical_memory": memory,
                "representation_cap_bytes": REPRESENTATION_CAP_BYTES,
                "process_peak_working_set_bytes": process_memory,
            },
            "hashes": extended_hashes(
                baseline, executable, metadata_path, manifest_path,
                expectations_path, bindings, generator, large_fixture,
                harness),
            "performance": performance,
            "payload": payload,
        }
        publish_report(output, report, deadline, protected, baseline)
        print(f"representation qualification report: {output}")
        return 0
    except (HarnessError, OSError, UnicodeError, json.JSONDecodeError,
            TypeError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
