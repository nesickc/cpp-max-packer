#!/usr/bin/env python3
"""Deterministic T-001 checks for the pinned Windows CPU build.

The tool only reports or rejects drift.  It never updates a committed lock.
"""
import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
from pathlib import Path


FORBIDDEN = ("vulkan", "cuda", "cudart", "nv", "hip", "rocm", "amdhip")
SYSTEM_IMPORTS = {
    "advapi32.dll", "apphelp.dll", "bcryptprimitives.dll", "combase.dll", "crypt32.dll",
    "cryptbase.dll", "gdi32.dll", "kernel32.dll", "kernelbase.dll",
    "msvcrt.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "rpcrt4.dll",
    "sechost.dll", "shell32.dll", "user32.dll", "ws2_32.dll",
}
TOOLCHAIN_FIELDS = ("architecture", "vs_version", "toolset", "compiler", "windows_sdk",
                    "cmake", "ninja", "triplet", "msvc_runtime")


def fail(messages):
    for message in messages:
        print(message, file=sys.stderr)
    return 1


def load_json(path):
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream)


def compare_fields(expected, actual, label, fields):
    errors = []
    for field in fields:
        if field not in expected:
            continue
        if actual.get(field) != expected[field]:
            errors.append(f"{label} {field} expected {expected[field]!r}, got {actual.get(field)!r}")
    return errors


def check_toolchain(args):
    profiles = load_json(args.lock).get("profiles", {})
    if args.profile not in profiles:
        return fail([f"unknown locked toolchain profile: {args.profile}"])
    errors = compare_fields(profiles[args.profile], load_json(args.actual), "toolchain", TOOLCHAIN_FIELDS)
    return fail(errors) if errors else 0


def indexed(entries, kind):
    result = {}
    for entry in entries:
        name = entry.get("name")
        if not name:
            raise ValueError(f"{kind} entry has no name")
        if name in result:
            raise ValueError(f"duplicate {kind} entry: {name}")
        result[name] = entry
    return result


def check_dependencies(args):
    lock, actual = load_json(args.lock), load_json(args.actual)
    errors = []
    for group in ("dependencies", "host_tools"):
        try:
            expected_items, actual_items = indexed(lock.get(group, []), group), indexed(actual.get(group, []), group)
        except ValueError as exc:
            return fail([str(exc)])
        for name, expected in expected_items.items():
            if name not in actual_items:
                errors.append(f"{group} {name} missing from installed resolution")
                continue
            errors.extend(compare_fields(expected, actual_items[name], name,
                                         ("version", "features", "triplet", "port_tree_hash", "source_sha512", "port_identity")))
        for name in actual_items.keys() - expected_items.keys():
            errors.append(f"undeclared installed {group} entry: {name}")
    return fail(errors) if errors else 0


def check_vcpkg_tool(args):
    expected = load_json(args.lock).get("vcpkg_tool", {})
    actual = load_json(args.actual)
    errors = []
    for field, actual_field in (("version", "vcpkg_tool_version"), ("sha256", "vcpkg_tool_sha256")):
        if actual.get(actual_field) != expected.get(field):
            errors.append(f"vcpkg_tool {field} expected {expected.get(field)!r}, got {actual.get(actual_field)!r}")
    return fail(errors) if errors else 0


def parse_status(path):
    """Read vcpkg's installed status without trusting generated CMake state."""
    paragraphs, current = [], {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines() + [""]:
        if not line.strip():
            if current.get("Package"):
                paragraphs.append(current)
            current = {}
        elif ":" in line:
            key, value = line.split(":", 1); current[key] = value.strip()
    installed = {}
    for paragraph in paragraphs:
        if paragraph.get("Status") != "install ok installed":
            continue
        key = (paragraph["Package"], paragraph.get("Architecture", ""))
        version = paragraph.get("Version", "")
        if paragraph.get("Port-Version", "0") != "0":
            version += f"#{paragraph['Port-Version']}"
        record = installed.setdefault(key, {"name": key[0], "version": version,
                                            "triplet": key[1], "features": []})
        if record["version"] != version:
            raise ValueError(f"installed status has conflicting versions for {key[0]}:{key[1]}")
        feature = paragraph.get("Feature")
        if feature and feature != "core" and feature not in record["features"]:
            record["features"].append(feature)
    for record in installed.values():
        record["features"].sort()
    return list(installed.values())


def collect_dependencies(args):
    installed = Path(args.installed_root)
    try:
        records = parse_status(args.status)
    except ValueError as exc:
        return fail([str(exc)])
    host, target = [], []
    for record in records:
        spdx_dir = record["name"].replace("-", "_") if record["name"] == "nlohmann-json" else record["name"]
        spdx = installed / record["triplet"] / "share" / spdx_dir / "vcpkg.spdx.json"
        if not spdx.exists():
            # A port may retain hyphens in share despite its CMake target spelling.
            spdx = installed / record["triplet"] / "share" / record["name"] / "vcpkg.spdx.json"
        if not spdx.exists():
            return fail([f"installed SPDX record missing for {record['name']}: {spdx}"])
        metadata = load_json(spdx)
        match = re.search(r"^([^:]+):([^@]+)@([^ ]+)", metadata.get("name", ""))
        if not match:
            return fail([f"unrecognised SPDX package identity for {record['name']}: {metadata.get('name')!r}"])
        if match.group(1) != record["name"] or match.group(2) != record["triplet"]:
            return fail([f"SPDX package identity does not match installed status for {record['name']}: {metadata.get('name')!r}"])
        record["version"] = match.group(3)
        record["port_identity"] = f"{match.group(1)}:{match.group(2)}@{match.group(3)}"
        packages = metadata.get("packages", [])
        port = next((item for item in packages if item.get("SPDXID") == "SPDXRef-port"), {})
        port_location = port.get("downloadLocation", "")
        port_hash = port_location.rsplit("@", 1)[-1] if "@" in port_location else ""
        primary_source = next((item for item in packages if item.get("SPDXID") == "SPDXRef-resource-0"), {})
        source_hashes = [checksum.get("checksumValue") for checksum in primary_source.get("checksums", [])
                         if checksum.get("algorithm") == "SHA512"]
        # Host-only CMake helpers carry no fetched source archive.  Product
        # ports must prove both the selected vcpkg port tree and archive.
        if record["triplet"] != args.host_triplet and (not re.fullmatch(r"[0-9a-f]{40}", port_hash) or not source_hashes):
            return fail([f"installed SPDX provenance missing port tree or SHA512 for {record['name']}: {spdx}"])
        if record["triplet"] != args.host_triplet:
            record["port_tree_hash"] = port_hash
            record["source_sha512"] = source_hashes[0]
        (host if record["triplet"] == args.host_triplet else target).append(record)
    Path(args.out).write_text(json.dumps({"dependencies": sorted(target, key=lambda x: x["name"]),
                                          "host_tools": sorted(host, key=lambda x: x["name"])}, indent=2) + "\n", encoding="utf-8")
    return 0


def is_system_module(path):
    normalized = str(path).replace("/", "\\").lower()
    return bool(re.match(r"^[a-z]:\\windows\\(system32|syswow64)\\[^\\]+$", normalized))


def read_imports(path):
    # dumpbin /imports output and a one-name-per-line fixture are both accepted.
    names = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        found = re.search(r"([A-Za-z0-9_.-]+\.dll)\b", line, re.IGNORECASE)
        if found:
            names.append(found.group(1).lower())
    return names


def audit_runtime(args):
    root = Path(args.package_root).resolve()
    report = load_json(args.runtime_report)
    errors = []
    if report.get("schema_version") != 1:
        errors.append("runtime report schema_version must be 1")
    if report.get("architecture") != "x64":
        errors.append("runtime report architecture must be x64")
    if report.get("msvc_runtime") != "static":
        errors.append("runtime report msvc_runtime must be static")
    if not report.get("compiler"):
        errors.append("runtime report compiler must be recorded")
    modules = report.get("modules", [])
    if not isinstance(modules, list) or not modules:
        errors.append("runtime report has no loaded modules")
        modules = []
    expected_exe = root / "diagnostics" / "spectrapack_cpu_dependency_smoke.exe"
    if not any(Path(module.get("path", "")).resolve() == expected_exe.resolve() for module in modules):
        errors.append(f"runtime report does not contain staged diagnostic: {expected_exe}")
    loaded_names = set()
    for module in modules:
        if not isinstance(module, dict):
            errors.append(f"runtime module entry is not an object: {module!r}")
            continue
        name, path = module.get("name", ""), module.get("path", "")
        lowered = name.lower()
        if not name:
            errors.append("runtime module has no name")
        elif lowered in loaded_names:
            errors.append(f"duplicate runtime module: {name}")
        else:
            loaded_names.add(lowered)
        if any(token in lowered for token in FORBIDDEN):
            errors.append(f"forbidden runtime module: {name}")
        if not path:
            errors.append(f"runtime module has no path: {name}")
            continue
        path_name = re.split(r"[\\/]", str(path))[-1].lower()
        if lowered and path_name != lowered:
            errors.append(f"runtime module name/path mismatch: {name} ({path})")
        if not is_system_module(path):
            try:
                within_package = Path(path).resolve().is_relative_to(root)
            except ValueError:
                within_package = False
            if not within_package:
                errors.append(f"undeclared non-system module outside package: {name} ({path})")
            elif lowered.endswith(".dll"):
                errors.append(f"unexpected packaged runtime module: {name}")
        elif lowered not in SYSTEM_IMPORTS:
            errors.append(f"undeclared system-directory module: {name}")
    imports = read_imports(args.pe_imports)
    if not imports:
        errors.append("PE import audit has no imports")
    for name in imports:
        if any(token in name for token in FORBIDDEN):
            errors.append(f"forbidden PE import: {name}")
        elif name not in SYSTEM_IMPORTS:
            errors.append(f"undeclared PE import: {name}")
        elif name not in loaded_names:
            errors.append(f"PE import missing from runtime modules: {name}")
    return fail(errors) if errors else 0


def check_crt(args):
    expected_runtime = "MultiThreadedDebug" if args.build_type.lower() == "debug" else "MultiThreaded"
    expected_flag = "/MTd" if args.build_type.lower() == "debug" else "/MT"
    cache = Path(args.cache).read_text(encoding="utf-8", errors="replace")
    errors = []
    runtime_value = None
    for line in cache.splitlines():
        if line.startswith("CMAKE_MSVC_RUNTIME_LIBRARY:") and "=" in line:
            runtime_value = line.split("=", 1)[1]
    accepted_runtime = {expected_runtime, "MultiThreaded$<$<CONFIG:Debug>:Debug>"}
    if runtime_value not in accepted_runtime:
        errors.append(f"CMake cache does not select static runtime; got {runtime_value!r}")
    commands = load_json(args.compile_commands)
    if not isinstance(commands, list) or not commands:
        errors.append("compile_commands.json contains no compile commands")
        commands = []
    for index, entry in enumerate(commands):
        command_text = entry.get("command", entry.get("arguments", "") if isinstance(entry.get("arguments", ""), str) else " ".join(entry.get("arguments", [])))
        flags = {flag[1:].lower() for flag in re.findall(r"(?i)(?<!\S)[/-](?:MTd|MT|MDd|MD)(?!\S)", command_text)}
        expected = expected_flag[1:].lower()
        if expected not in flags:
            errors.append(f"compile command {index} does not contain required {expected_flag}")
        if any(flag.startswith("md") or flag != expected for flag in flags):
            errors.append(f"compile command {index} has conflicting CRT flags: {sorted(flags)}")
    return fail(errors) if errors else 0


def parse_cmake_compiler_record(path):
    values = {}
    for name, value in re.findall(r'^set\((CMAKE_CXX_[A-Z0-9_]+) "([^\"]*)"\)',
                                  Path(path).read_text(encoding="utf-8", errors="replace"), re.MULTILINE):
        values[name] = value
    required = ("CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID",
                "CMAKE_CXX_COMPILER_VERSION", "CMAKE_CXX_COMPILER_ARCHITECTURE_ID")
    missing = [name for name in required if not values.get(name)]
    if missing:
        raise ValueError(f"configured compiler record is missing: {', '.join(missing)}")
    return values


def windows_file_version(path):
    if os.name != "nt":
        raise RuntimeError("configured compiler file version measurement requires Windows")
    size = ctypes.windll.version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        raise RuntimeError(f"configured compiler has no readable Windows file version: {path}")
    buffer = ctypes.create_string_buffer(size)
    if not ctypes.windll.version.GetFileVersionInfoW(str(path), 0, size, buffer):
        raise RuntimeError(f"could not read configured compiler file version: {path}")
    pointer, length = ctypes.c_void_p(), ctypes.c_uint()
    if not ctypes.windll.version.VerQueryValueW(
            buffer, "\\VarFileInfo\\Translation", ctypes.byref(pointer), ctypes.byref(length)) or length.value < 4:
        raise RuntimeError(f"configured compiler version translation is missing: {path}")
    translation = ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint32)).contents.value
    language, codepage = translation & 0xffff, translation >> 16
    query = f"\\StringFileInfo\\{language:04x}{codepage:04x}\\FileVersion"
    if not ctypes.windll.version.VerQueryValueW(buffer, query, ctypes.byref(pointer), ctypes.byref(length)):
        raise RuntimeError(f"configured compiler file version record is missing: {path}")
    version = ctypes.wstring_at(pointer).strip()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
        raise RuntimeError(f"configured compiler file version is invalid: {version!r}")
    return version


def normalized_path(path):
    return os.path.normcase(os.path.abspath(path))


def record_build(args):
    metadata = load_json(args.metadata)
    cache_lines = Path(args.cache).read_text(encoding="utf-8", errors="replace").splitlines()
    cache = {line.split(":", 1)[0]: line.split("=", 1)[1] for line in cache_lines if "=" in line and line.startswith("CMAKE_")}
    try:
        metadata["source_revision"] = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        metadata["source_revision"] = "unavailable"
    try:
        compiler = parse_cmake_compiler_record(args.compiler_record)
        configured_compiler = cache.get("CMAKE_CXX_COMPILER")
        configured_ninja = cache.get("CMAKE_MAKE_PROGRAM")
        if not configured_compiler or not configured_ninja:
            raise ValueError("CMake cache does not record configured compiler and Ninja paths")
        if normalized_path(configured_compiler) != normalized_path(compiler["CMAKE_CXX_COMPILER"]):
            raise ValueError("configured compiler does not match CMake compiler record: "
                             f"{configured_compiler!r} != {compiler['CMAKE_CXX_COMPILER']!r}")
        if compiler["CMAKE_CXX_COMPILER_ID"] != "MSVC":
            raise ValueError(f"configured compiler must be MSVC; got {compiler['CMAKE_CXX_COMPILER_ID']!r}")
        compiler_file_version = windows_file_version(configured_compiler)
        if compiler_file_version != compiler["CMAKE_CXX_COMPILER_VERSION"]:
            raise ValueError("configured compiler file version does not match CMake compiler record: "
                             f"{compiler_file_version!r} != {compiler['CMAKE_CXX_COMPILER_VERSION']!r}")
        configured_ninja_version = command_version(
            [configured_ninja, "--version"], r"(?m)^([0-9]+(?:\.[0-9]+)+)\s*$")
    except (OSError, RuntimeError, ValueError) as exc:
        return fail([f"configured toolchain measurement failed: {exc}"])
    metadata["profile"] = args.profile
    metadata["build_type"] = args.build_type
    metadata["architecture"] = compiler["CMAKE_CXX_COMPILER_ARCHITECTURE_ID"]
    metadata["native_architecture"] = compiler["CMAKE_CXX_COMPILER_ARCHITECTURE_ID"]
    metadata["compiler"] = compiler["CMAKE_CXX_COMPILER_VERSION"]
    metadata["compiler_file_version"] = compiler_file_version
    metadata["configured_compiler"] = configured_compiler
    metadata["ninja"] = configured_ninja_version
    metadata["configured_ninja"] = configured_ninja
    metadata["build_flags"] = {key: value for key, value in cache.items()
                               if key.startswith("CMAKE_MSVC") or key in ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_DEBUG", "CMAKE_CXX_FLAGS_RELEASE")}
    Path(args.metadata).write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return 0


def command_version(command, pattern):
    # MSVC's banner invocation can use exit code 2 even though it printed a
    # usable version banner; parse the banner rather than treating that as I/O.
    output = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    found = re.search(pattern, output)
    if not found:
        raise RuntimeError(f"could not read version from {' '.join(command)}: {output}")
    return found.group(1)


def capture_toolchain(args):
    try:
        record = {
            "architecture": os.environ.get("VSCMD_ARG_TGT_ARCH", ""), "vs_version": args.vs_version,
            "toolset": os.environ.get("VCToolsVersion", "").rstrip("\\/"), "compiler": command_version(["cl"], r"Version ([0-9.]+)"),
            "windows_sdk": os.environ.get("WindowsSDKVersion", "").rstrip("\\/"), "cmake": command_version([args.cmake, "--version"], r"version ([0-9.]+)"),
            "ninja": command_version([args.ninja, "--version"], r"([0-9.]+)"),
            "triplet": args.triplet, "msvc_runtime": "static", "vcpkg_tool_version": args.vcpkg_tool_version,
            "vcpkg_tool_sha256": args.vcpkg_tool_sha256,
        }
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        return fail([f"toolchain capture failed: {exc}"])
    Path(args.out).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return 0


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    toolchain = commands.add_parser("check-toolchain")
    toolchain.add_argument("--lock", required=True); toolchain.add_argument("--profile", required=True); toolchain.add_argument("--actual", required=True)
    dependencies = commands.add_parser("check-dependencies")
    dependencies.add_argument("--lock", required=True); dependencies.add_argument("--actual", required=True)
    vcpkg_tool = commands.add_parser("check-vcpkg-tool")
    vcpkg_tool.add_argument("--lock", required=True); vcpkg_tool.add_argument("--actual", required=True)
    collected = commands.add_parser("collect-dependencies")
    collected.add_argument("--status", required=True); collected.add_argument("--installed-root", required=True); collected.add_argument("--out", required=True)
    collected.add_argument("--host-triplet", default="x64-windows")
    runtime = commands.add_parser("audit-runtime")
    runtime.add_argument("--package-root", required=True); runtime.add_argument("--runtime-report", required=True); runtime.add_argument("--pe-imports", required=True)
    crt = commands.add_parser("check-crt")
    crt.add_argument("--build-type", choices=("Debug", "Release"), required=True)
    crt.add_argument("--cache", required=True); crt.add_argument("--compile-commands", required=True)
    record = commands.add_parser("record-build")
    record.add_argument("--metadata", required=True); record.add_argument("--cache", required=True)
    record.add_argument("--compiler-record", required=True)
    record.add_argument("--profile", required=True); record.add_argument("--build-type", choices=("Debug", "Release"), required=True)
    capture = commands.add_parser("capture-toolchain")
    capture.add_argument("--out", required=True); capture.add_argument("--vs-version", required=True)
    capture.add_argument("--triplet", default="x64-windows-static")
    capture.add_argument("--vcpkg-tool-version", required=True); capture.add_argument("--vcpkg-tool-sha256", required=True); capture.add_argument("--cmake", default="cmake"); capture.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    return {"check-toolchain": check_toolchain, "check-dependencies": check_dependencies, "check-vcpkg-tool": check_vcpkg_tool, "collect-dependencies": collect_dependencies,
            "audit-runtime": audit_runtime, "check-crt": check_crt, "record-build": record_build, "capture-toolchain": capture_toolchain}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
