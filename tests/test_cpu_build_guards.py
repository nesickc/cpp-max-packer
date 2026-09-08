import json
import platform
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
TOOL = ROOT / "tools" / "Inspect-CpuBuild.py"


class CpuBuildGuardTests(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *map(str, args)], text=True,
                              capture_output=True, cwd=ROOT)

    def write_json(self, directory, name, value):
        path = Path(directory) / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def write_pocketfft_spdx(self, installed_root, *, port_hash="4" * 40,
                             source_hash="e" * 128, identity="pocketfft:x64-windows-static@2023-09-25"):
        spdx = Path(installed_root) / "x64-windows-static" / "share" / "pocketfft" / "vcpkg.spdx.json"
        spdx.parent.mkdir(parents=True, exist_ok=True)
        spdx.write_text(json.dumps({
            "name": identity,
            "packages": [
                {"name": "pocketfft", "SPDXID": "SPDXRef-port",
                 "downloadLocation": f"git+https://github.com/Microsoft/vcpkg@{port_hash}"},
                {"name": "pocketfft:x64-windows-static", "SPDXID": "SPDXRef-binary",
                 "downloadLocation": "NONE"},
                {"name": "mreineck/pocketfft", "SPDXID": "SPDXRef-resource-0",
                 "downloadLocation": "git+https://github.com/mreineck/pocketfft@revision",
                 "checksums": [{"algorithm": "SHA512", "checksumValue": source_hash}]},
            ],
        }), encoding="utf-8")
        return spdx

    def test_exact_toolchain_profile_accepts_match_and_rejects_compiler_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = self.write_json(directory, "toolchains.json", {"profiles": {"local": {
                "architecture": "x64", "vs_version": "18.8.12105.206", "toolset": "14.51.36231",
                "compiler": "19.51.36252.0", "windows_sdk": "10.0.26100.0",
                "cmake": "4.2.3", "ninja": "1.13.2", "triplet": "x64-windows-static",
                "msvc_runtime": "static"}}})
            actual = {"architecture": "x64", "vs_version": "18.8.12105.206", "toolset": "14.51.36231",
                      "compiler": "19.51.36252.0", "windows_sdk": "10.0.26100.0", "cmake": "4.2.3",
                      "ninja": "1.13.2", "triplet": "x64-windows-static", "msvc_runtime": "static"}
            actual_path = self.write_json(directory, "actual.json", actual)
            self.assertEqual(self.run_tool("check-toolchain", "--lock", lock, "--profile", "local", "--actual", actual_path).returncode, 0)
            actual["compiler"] = "19.51.99999.0"
            actual_path.write_text(json.dumps(actual), encoding="utf-8")
            result = self.run_tool("check-toolchain", "--lock", lock, "--profile", "local", "--actual", actual_path)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("compiler expected '19.51.36252.0', got '19.51.99999.0'", result.stderr)

    def test_dependency_lock_rejects_version_feature_triplet_and_source_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = {"schema_version": 1, "triplet": "x64-windows-static", "dependencies": [{
                "name": "pocketfft", "version": "2023-09-25", "features": [], "triplet": "x64-windows-static",
                "port_tree_hash": "port-a", "source_sha512": "source-a"}], "host_tools": [{
                "name": "vcpkg-cmake", "version": "2025-08-07", "triplet": "x64-windows"}]}
            actual = {"dependencies": [{"name": "pocketfft", "version": "2023-09-25", "features": [],
                "triplet": "x64-windows-static", "port_tree_hash": "port-a", "source_sha512": "source-a"}],
                "host_tools": [{"name": "vcpkg-cmake", "version": "2025-08-07", "triplet": "x64-windows"}]}
            lock_path = self.write_json(directory, "dependencies.json", lock)
            actual_path = self.write_json(directory, "actual.json", actual)
            self.assertEqual(self.run_tool("check-dependencies", "--lock", lock_path, "--actual", actual_path).returncode, 0)
            for field, bad in (("version", "2024-01-01"), ("features", ["avx"]),
                               ("triplet", "x64-windows"), ("source_sha512", "source-b")):
                broken = json.loads(json.dumps(actual)); broken["dependencies"][0][field] = bad
                actual_path.write_text(json.dumps(broken), encoding="utf-8")
                result = self.run_tool("check-dependencies", "--lock", lock_path, "--actual", actual_path)
                self.assertNotEqual(result.returncode, 0, field)
                self.assertIn("pocketfft", result.stderr)
                self.assertIn(field, result.stderr)
            missing = json.loads(json.dumps(actual)); del missing["dependencies"][0]["source_sha512"]
            actual_path.write_text(json.dumps(missing), encoding="utf-8")
            result = self.run_tool("check-dependencies", "--lock", lock_path, "--actual", actual_path)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source_sha512", result.stderr)

    def test_collected_vcpkg_status_and_spdx_provenance_reject_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            installed = Path(directory) / "installed"
            status = Path(directory) / "status"
            status.write_text(
                "Package: catch2\nVersion: 3.11.0\nArchitecture: x64-windows\n"
                "Status: purge ok not-installed\n\n"
                "Package: pocketfft\nVersion: 2023-09-25\nArchitecture: x64-windows-static\n"
                "Status: install ok installed\n\n",
                encoding="utf-8")
            spdx = self.write_pocketfft_spdx(installed)
            actual = Path(directory) / "actual.json"
            result = self.run_tool("collect-dependencies", "--status", status,
                                   "--installed-root", installed, "--out", actual)
            self.assertEqual(result.returncode, 0, result.stderr)
            collected = json.loads(actual.read_text(encoding="utf-8"))
            self.assertEqual([entry["name"] for entry in collected["dependencies"]], ["pocketfft"])
            lock = self.write_json(directory, "lock.json", collected)
            self.assertEqual(self.run_tool("check-dependencies", "--lock", lock,
                                           "--actual", actual).returncode, 0)

            for field, mutation in (
                ("source_sha512", {"source_hash": "f" * 128}),
                ("port_tree_hash", {"port_hash": "5" * 40}),
                ("port_identity", {"identity": "pocketfft:x64-windows-static@2024-01-01"}),
            ):
                self.write_pocketfft_spdx(installed, **mutation)
                result = self.run_tool("collect-dependencies", "--status", status,
                                       "--installed-root", installed, "--out", actual)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = self.run_tool("check-dependencies", "--lock", lock, "--actual", actual)
                self.assertNotEqual(result.returncode, 0, field)
                self.assertIn(field, result.stderr)
                spdx = self.write_pocketfft_spdx(installed)

            status.write_text(status.read_text(encoding="utf-8") +
                              "Package: pocketfft\nFeature: avx\nVersion: 2023-09-25\n"
                              "Architecture: x64-windows-static\nStatus: install ok installed\n\n",
                              encoding="utf-8")
            result = self.run_tool("collect-dependencies", "--status", status,
                                   "--installed-root", installed, "--out", actual)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.run_tool("check-dependencies", "--lock", lock, "--actual", actual)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("features", result.stderr)

    def test_runtime_audit_rejects_forbidden_and_external_modules_but_accepts_package_and_system(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "stage"; (root / "diagnostics").mkdir(parents=True)
            exe = root / "diagnostics" / "spectrapack_cpu_dependency_smoke.exe"; exe.write_bytes(b"MZ")
            report = {"schema_version": 1, "architecture": "x64", "compiler": "MSVC", "msvc_runtime": "static",
                      "modules": [{"name": "spectrapack_cpu_dependency_smoke.exe", "path": str(exe)},
                                  {"name": "kernel32.dll", "path": "C:/Windows/System32/kernel32.dll"}]}
            report_path = self.write_json(directory, "report.json", report)
            imports = Path(directory) / "imports.txt"; imports.write_text("KERNEL32.dll\n", encoding="utf-8")
            self.assertEqual(self.run_tool("audit-runtime", "--package-root", root, "--runtime-report", report_path,
                                           "--pe-imports", imports).returncode, 0)
            report["modules"].append({"name": "vulkan-1.dll", "path": "C:/elsewhere/vulkan-1.dll"})
            report_path.write_text(json.dumps(report), encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root, "--runtime-report", report_path, "--pe-imports", imports)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("forbidden runtime module: vulkan-1.dll", result.stderr)
            report["modules"].pop(); report["modules"].append({"name": "thirdparty.dll", "path": "C:/elsewhere/thirdparty.dll"})
            report_path.write_text(json.dumps(report), encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root, "--runtime-report", report_path, "--pe-imports", imports)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("undeclared non-system module", result.stderr)

    def test_crt_audit_requires_static_cache_and_compile_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            commands = Path(directory) / "compile_commands.json"
            cache.write_text("CMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded$<$<CONFIG:Debug>:Debug>\n", encoding="utf-8")
            commands.write_text(json.dumps([{ "command": "cl.exe /nologo /MTd /c probe.cpp" }, {"command": "cl.exe -MTd /c other.cpp"}]), encoding="utf-8")
            self.assertEqual(self.run_tool("check-crt", "--build-type", "Debug", "--cache", cache,
                                           "--compile-commands", commands).returncode, 0)
            commands.write_text(json.dumps([{ "command": "cl.exe /nologo /MDd /c probe.cpp" }]), encoding="utf-8")
            result = self.run_tool("check-crt", "--build-type", "Debug", "--cache", cache,
                                   "--compile-commands", commands)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("does not contain required /MTd", result.stderr)
            cache.write_text("CMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded$<$<CONFIG:Debug>:Debug>\n", encoding="utf-8")
            commands.write_text(json.dumps([{ "command": "cl.exe -MT /c release.cpp" }, {"command":"cl.exe -MTd /c wrong.cpp"}]), encoding="utf-8")
            result = self.run_tool("check-crt", "--build-type", "Release", "--cache", cache, "--compile-commands", commands)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("conflicting CRT", result.stderr)
            commands.write_text("[]", encoding="utf-8")
            result = self.run_tool("check-crt", "--build-type", "Release", "--cache", cache,
                                   "--compile-commands", commands)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no compile commands", result.stderr)

    def test_record_build_uses_configured_cmake_compiler_record_and_ninja(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            record_dir = build / "CMakeFiles" / "4.2.3"
            record_dir.mkdir(parents=True)
            compiler = Path(sys.executable)
            compiler_record = record_dir / "CMakeCXXCompiler.cmake"
            compiler_record.write_text(
                f'set(CMAKE_CXX_COMPILER "{compiler.as_posix()}")\n'
                'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                f'set(CMAKE_CXX_COMPILER_VERSION "{platform.python_version()}")\n'
                'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "x64")\n', encoding="utf-8")
            ninja = Path(directory) / "ninja.cmd"
            ninja.write_text("@echo 1.13.2\n", encoding="utf-8")
            cache = build / "CMakeCache.txt"
            cache.write_text(
                f"CMAKE_CXX_COMPILER:FILEPATH={compiler.as_posix()}\n"
                f"CMAKE_MAKE_PROGRAM:FILEPATH={ninja.as_posix()}\n",
                encoding="utf-8")
            metadata = self.write_json(directory, "metadata.json", {
                "architecture": "x64", "compiler": "19.51.36252", "ninja": "wrong"})
            result = self.run_tool("record-build", "--metadata", metadata, "--cache", cache,
                                   "--compiler-record", compiler_record,
                                   "--profile", "local", "--build-type", "Debug")
            self.assertEqual(result.returncode, 0, result.stderr)
            recorded = json.loads(metadata.read_text(encoding="utf-8"))
            self.assertEqual(recorded["compiler"], platform.python_version())
            self.assertEqual(recorded["compiler_file_version"], platform.python_version())
            self.assertEqual(recorded["architecture"], "x64")
            self.assertEqual(recorded["native_architecture"], "x64")
            self.assertEqual(recorded["ninja"], "1.13.2")
            self.assertEqual(Path(recorded["configured_compiler"]).resolve(), compiler.resolve())
            self.assertEqual(Path(recorded["configured_ninja"]).resolve(), ninja.resolve())

            cache.write_text(
                f"CMAKE_CXX_COMPILER:FILEPATH={(Path(directory) / 'other-cl.exe').as_posix()}\n"
                f"CMAKE_MAKE_PROGRAM:FILEPATH={ninja.as_posix()}\n",
                encoding="utf-8")
            result = self.run_tool("record-build", "--metadata", metadata, "--cache", cache,
                                   "--compiler-record", compiler_record,
                                   "--profile", "local", "--build-type", "Debug")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("configured compiler does not match", result.stderr)

    def test_runtime_audit_requires_complete_known_system_closure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "stage"; (root / "diagnostics").mkdir(parents=True)
            report = self.write_json(directory, "report.json", {"schema_version": 1, "architecture": "x64", "msvc_runtime": "static", "modules": []})
            imports = Path(directory) / "imports.txt"; imports.write_text("", encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root, "--runtime-report", report, "--pe-imports", imports)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no loaded modules", result.stderr)
            report.write_text(json.dumps({"schema_version": 1, "architecture": "x64", "msvc_runtime": "static", "modules": [{"name":"x.dll", "path":"C:/Temp/x.dll"}]}), encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root, "--runtime-report", report, "--pe-imports", imports)
            self.assertIn("undeclared non-system module", result.stderr)
            report.write_text(json.dumps({"schema_version":1,"architecture":"x64","msvc_runtime":"static","modules":[{"name":"spectrapack_cpu_dependency_smoke.exe","path":str(root/'diagnostics'/'spectrapack_cpu_dependency_smoke.exe')},{"name":"thirdparty.dll","path":"C:/Windows/System32/thirdparty.dll"}]}), encoding="utf-8")
            imports.write_text("thirdparty.dll\n", encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root, "--runtime-report", report, "--pe-imports", imports)
            self.assertIn("undeclared system-directory module", result.stderr)
            self.assertIn("undeclared PE import", result.stderr)

    def test_runtime_audit_binds_imports_to_loaded_module_names_and_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "stage"
            (root / "diagnostics").mkdir(parents=True)
            exe = root / "diagnostics" / "spectrapack_cpu_dependency_smoke.exe"
            exe.write_bytes(b"MZ")
            report_path = self.write_json(directory, "report.json", {
                "schema_version": 1, "architecture": "x64", "compiler": "MSVC 19.51.36252.0",
                "msvc_runtime": "static", "modules": [
                    {"name": exe.name, "path": str(exe)},
                    {"name": "kernel32.dll", "path": "C:/Windows/System32/kernel32.dll"},
                    {"name": "apphelp.dll", "path": "C:/Windows/System32/apphelp.dll"},
                    {"name": "msvcrt.dll", "path": "C:/Windows/System32/msvcrt.dll"},
                    {"name": "sechost.dll", "path": "C:/Windows/System32/sechost.dll"},
                    {"name": "rpcrt4.dll", "path": "C:/Windows/System32/rpcrt4.dll"},
                    {"name": "cryptbase.dll", "path": "C:/Windows/System32/cryptbase.dll"},
                    {"name": "bcryptPrimitives.dll", "path": "C:/Windows/System32/bcryptPrimitives.dll"},
                ]})
            imports = Path(directory) / "imports.txt"
            imports.write_text("kernel32.dll\n", encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root,
                                   "--runtime-report", report_path, "--pe-imports", imports)
            self.assertEqual(result.returncode, 0, result.stderr)

            cases = [
                ({"name": "kernel32.dll", "path": "C:/Windows/Temp/kernel32.dll"},
                 "undeclared non-system module"),
                ({"name": "thirdparty.dll", "path": "C:/Windows/System32/thirdparty.dll"},
                 "undeclared system-directory module"),
                ({"name": "kernel32.dll", "path": "C:/Windows/System32/user32.dll"},
                 "module name/path mismatch"),
            ]
            for module, expected in cases:
                report = json.loads(report_path.read_text(encoding="utf-8"))
                report["modules"][1] = module
                report_path.write_text(json.dumps(report), encoding="utf-8")
                result = self.run_tool("audit-runtime", "--package-root", root,
                                       "--runtime-report", report_path, "--pe-imports", imports)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                report_path.write_text(json.dumps({
                    "schema_version": 1, "architecture": "x64", "compiler": "MSVC 19.51.36252.0",
                    "msvc_runtime": "static", "modules": [
                        {"name": exe.name, "path": str(exe)},
                        {"name": "kernel32.dll", "path": "C:/Windows/System32/kernel32.dll"},
                    ]}), encoding="utf-8")

            imports.write_text("kernel32.dll\nuser32.dll\n", encoding="utf-8")
            result = self.run_tool("audit-runtime", "--package-root", root,
                                   "--runtime-report", report_path, "--pe-imports", imports)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("PE import missing from runtime modules: user32.dll", result.stderr)

    def test_vcpkg_binary_identity_is_checked_separately_from_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = self.write_json(directory, "dependencies.json", {"vcpkg_tool": {
                "version": "2025-10-16-abc", "sha256": "a" * 64}})
            actual = self.write_json(directory, "metadata.json", {"vcpkg_tool_version": "2025-10-16-abc",
                "vcpkg_tool_sha256": "a" * 64})
            self.assertEqual(self.run_tool("check-vcpkg-tool", "--lock", lock, "--actual", actual).returncode, 0)
            actual.write_text(json.dumps({"vcpkg_tool_version": "2025-10-16-abc", "vcpkg_tool_sha256": "b" * 64}), encoding="utf-8")
            result = self.run_tool("check-vcpkg-tool", "--lock", lock, "--actual", actual)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("vcpkg_tool sha256", result.stderr)


if __name__ == "__main__":
    unittest.main()
