"""Black-box CLI controls for retained failures and caller-reserve accounting.

These cases exercise a small analytic cube through the executable.  They do not
depend on the artifact suite's fixture helpers because CTest invokes selectors
independently.
"""

import argparse
import ctypes
import json
import math
import os
import pathlib
import queue
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import unittest


ENGINE: pathlib.Path
FIXED_CATALOG_SHA256 = "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240"
NEAR_UNIT_CUSTOM_SHA256 = "15cb4952f292b8f531d9bf01aefda8ce2998eb8b9cf4a4675d35282861c0a999"
FACES = (
    (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
    (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
)


def run_engine(*arguments: pathlib.Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(ENGINE), *(str(argument) for argument in arguments)],
        capture_output=True,
        encoding="utf-8",
        text=True,
        timeout=25,
    )


def write_cube(path: pathlib.Path) -> None:
    vertices = (
        (0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0),
        (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10),
    )
    payload = bytearray(80)
    payload.extend(struct.pack("<I", len(FACES)))
    for face in FACES:
        payload.extend(struct.pack("<3f", 0, 0, 0))
        for index in face:
            payload.extend(struct.pack("<3f", *vertices[index]))
        payload.extend(struct.pack("<H", 0))
    path.write_bytes(payload)


def hidden_startupinfo() -> subprocess.STARTUPINFO:
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    return startup


class JobBasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("per_process_user_time_limit", ctypes.c_int64),
        ("per_job_user_time_limit", ctypes.c_int64),
        ("limit_flags", ctypes.c_uint32),
        ("minimum_working_set_size", ctypes.c_size_t),
        ("maximum_working_set_size", ctypes.c_size_t),
        ("active_process_limit", ctypes.c_uint32),
        ("affinity", ctypes.c_size_t),
        ("priority_class", ctypes.c_uint32),
        ("scheduling_class", ctypes.c_uint32),
    ]


class IoCounters(ctypes.Structure):
    _fields_ = [
        ("read_operation_count", ctypes.c_uint64),
        ("write_operation_count", ctypes.c_uint64),
        ("other_operation_count", ctypes.c_uint64),
        ("read_transfer_count", ctypes.c_uint64),
        ("write_transfer_count", ctypes.c_uint64),
        ("other_transfer_count", ctypes.c_uint64),
    ]


class JobExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("basic_limit_information", JobBasicLimitInformation),
        ("io_info", IoCounters),
        ("process_memory_limit", ctypes.c_size_t),
        ("job_memory_limit", ctypes.c_size_t),
        ("peak_process_memory_used", ctypes.c_size_t),
        ("peak_job_memory_used", ctypes.c_size_t),
    ]


def validated_readiness_count(publication: str) -> int:
    match = re.fullmatch(r"solve: validated count=([0-9]+)\r?\n?", publication)
    if match is None:
        raise AssertionError(f"solve readiness count is missing or malformed: {publication!r}")
    return int(match[1])


def isolated_console_process(command: list[str], send_break: bool,
                             readiness_timeout: float) -> tuple[int, str, str]:
    """Run an owned process tree and optionally signal it after bounded readiness."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    kernel32.SetInformationJobObject.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.SetInformationJobObject.restype = ctypes.c_int
    kernel32.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.AssignProcessToJobObject.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.FreeConsole.argtypes = []
    kernel32.FreeConsole.restype = ctypes.c_int
    kernel32.AttachConsole.argtypes = [ctypes.c_uint32]
    kernel32.AttachConsole.restype = ctypes.c_int
    kernel32.SetConsoleCtrlHandler.argtypes = [ctypes.c_void_p, ctypes.c_int]
    kernel32.SetConsoleCtrlHandler.restype = ctypes.c_int
    kernel32.GenerateConsoleCtrlEvent.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    kernel32.GenerateConsoleCtrlEvent.restype = ctypes.c_int

    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = JobExtendedLimitInformation()
    limits.basic_limit_information.limit_flags = 0x00002000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not kernel32.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        error = ctypes.WinError(ctypes.get_last_error())
        kernel32.CloseHandle(job)
        raise error

    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.CREATE_NEW_PROCESS_GROUP,
            startupinfo=hidden_startupinfo(),
        )
    except Exception:
        kernel32.CloseHandle(job)
        raise
    if not kernel32.AssignProcessToJobObject(job, ctypes.c_void_p(process._handle)):
        error = ctypes.WinError(ctypes.get_last_error())
        process.kill()
        process.wait(timeout=5)
        kernel32.CloseHandle(job)
        raise error
    stdout = stderr = ""
    publication = ""
    attached = False
    try:
        if process.stderr is None:
            raise AssertionError("engine stderr pipe was not created")
        readiness: queue.Queue[tuple[bool, str]] = queue.Queue(maxsize=1)

        def read_readiness() -> None:
            try:
                readiness.put((True, process.stderr.readline()))
            except Exception as error:  # pragma: no cover - diagnostic transport
                readiness.put((False, repr(error)))

        threading.Thread(target=read_readiness, daemon=True).start()
        try:
            read_ok, publication = readiness.get(timeout=readiness_timeout)
        except queue.Empty as error:
            raise TimeoutError("solve readiness timed out") from error
        if not read_ok:
            raise AssertionError(f"solve readiness read failed: {publication}")
        validated_readiness_count(publication)
        if send_break:
            kernel32.FreeConsole()
            if not kernel32.AttachConsole(process.pid):
                raise ctypes.WinError(ctypes.get_last_error())
            attached = True
            if not kernel32.SetConsoleCtrlHandler(None, True):
                raise ctypes.WinError(ctypes.get_last_error())
            if not kernel32.GenerateConsoleCtrlEvent(signal.CTRL_BREAK_EVENT, process.pid):
                raise ctypes.WinError(ctypes.get_last_error())
        stdout, stderr = process.communicate(timeout=15)
    finally:
        if attached:
            kernel32.SetConsoleCtrlHandler(None, False)
            kernel32.FreeConsole()
        kernel32.CloseHandle(job)
        if process.poll() is None:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    return process.returncode, stdout, publication + stderr


def console_helper(engine: pathlib.Path, settings_path: pathlib.Path, object_report: pathlib.Path,
                   result_path: pathlib.Path) -> int:
    """Run solve in a hidden, isolated console and send its process group Ctrl-Break."""
    returncode, stdout, stderr = isolated_console_process(
        [str(engine), "solve", "--settings", str(settings_path), "--object-report", str(object_report),
         "--result", str(result_path)],
        send_break=True,
        readiness_timeout=5,
    )
    sys.stdout.write(stdout)
    sys.stderr.write(stderr)
    return returncode


def no_readiness_helper(pid_path: pathlib.Path) -> int:
    child = (
        "import os,pathlib,sys,time; "
        "pathlib.Path(sys.argv[1]).write_text(str(os.getpid()), encoding='ascii'); "
        "time.sleep(60)"
    )
    try:
        isolated_console_process(
            [sys.executable, "-c", child, str(pid_path)],
            send_break=False,
            readiness_timeout=1,
        )
    except TimeoutError as error:
        sys.stderr.write(f"{error}\n")
        return 0
    raise AssertionError("synthetic child unexpectedly reported readiness")


def process_exited(pid: int, timeout_ms: int) -> bool:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return ctypes.get_last_error() == 87  # ERROR_INVALID_PARAMETER: PID is gone.
    try:
        return kernel32.WaitForSingleObject(handle, timeout_ms) == 0
    finally:
        kernel32.CloseHandle(handle)


class SolveControls(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "cube.stl"
        self.object_report = self.root / "object.json"
        write_cube(self.source)
        inspected = run_engine(
            "inspect", "--stl", self.source, "--units", "mm", "--role", "object",
            "--report", self.object_report,
        )
        self.assertEqual(inspected.returncode, 0, (inspected.stdout, inspected.stderr))
        self.original_source = self.source.read_bytes()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def settings(self, preset: str) -> dict:
        asset = json.loads(self.object_report.read_text(encoding="utf-8"))
        return {
            "settings_version": 2,
            "object_asset": {
                "source_sha256": asset["source"]["sha256"],
                "accepted_solid_sha256": asset["accepted_solid"]["sha256"],
            },
            # Exact one-copy capacity keeps the physical seed count-dominated.
            "container": {"kind": "box", "dimensions_mm": [10, 10, 10]},
            "clearance_mm": {"pair": 0, "wall": 0},
            "orientation": {"mode": "fixed", "quaternion_xyzw": [0, 0, 0, 1]},
            "search": {"preset": preset, "seed": "0", "budget_seconds": 5},
            "resolution": {"mode": "manual", "pitch_mm": 1e-9},
            "compute": {"backend": "cpu"},
            "resolved": {
                "pitch_mm": 1e-9,
                "orientation_catalog_sha256": FIXED_CATALOG_SHA256,
                "orientation_catalog_version": 1,
                "backend": "cpu",
                "thread_count": 1,
            },
        }

    def timed_settings(self, budget_seconds: float) -> dict:
        settings = self.settings("deadline")
        settings["search"] = {
            "preset": "deadline",
            "seed": "0",
            "budget_seconds": budget_seconds,
        }
        settings["resolution"]["pitch_mm"] = 1
        settings["resolved"]["pitch_mm"] = 1
        settings["container"]["dimensions_mm"] = [40, 40, 40]
        return settings

    def stopping_settings(self) -> dict:
        settings = self.settings("console-stop")
        settings["search"]["budget_seconds"] = 60
        settings["resolution"]["pitch_mm"] = 1
        settings["resolved"]["pitch_mm"] = 1
        settings["container"]["dimensions_mm"] = [20, 10, 10]
        return settings

    def solve(self, preset: str, output_name: str) -> tuple[subprocess.CompletedProcess[str], pathlib.Path]:
        return self.solve_settings(self.settings(preset), output_name)

    def solve_settings(self, settings: dict, output_name: str) -> tuple[subprocess.CompletedProcess[str], pathlib.Path]:
        output = self.root / output_name
        output.mkdir()
        settings_path = output / "settings.json"
        result_path = output / "result.json"
        settings_path.write_text(json.dumps(settings), encoding="utf-8")
        completed = run_engine(
            "solve", "--settings", settings_path, "--object-report", self.object_report,
            "--result", result_path,
        )
        return completed, result_path

    def test_deadline(self) -> None:
        output = self.root / "deadline"
        output.mkdir()
        settings_path = output / "settings.json"
        result_path = output / "result.json"
        settings_path.write_text(json.dumps(self.timed_settings(0.1)), encoding="utf-8")

        completed = run_engine(
            "solve", "--settings", settings_path, "--object-report", self.object_report,
            "--result", result_path,
        )

        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        response = json.loads(completed.stdout)
        self.assertEqual(response["termination_reason"], "budget_exhausted")
        self.assertGreater(response["diagnostics"]["work"]["candidate_evaluations"], 0)
        self.assertTrue(result_path.is_file())

        enormous_path = output / "enormous-settings.json"
        enormous_result = output / "enormous-result.json"
        enormous_path.write_text(json.dumps(self.timed_settings(1e300)), encoding="utf-8")
        enormous = run_engine(
            "solve", "--settings", enormous_path, "--object-report", self.object_report,
            "--result", enormous_result,
        )
        self.assertEqual(enormous.returncode, 2, (enormous.stdout, enormous.stderr))
        self.assertEqual(json.loads(enormous.stdout)["error"]["code"], "INVALID_SETTINGS")
        self.assertFalse(enormous_result.exists())

        for seconds in (9223372036.854776, math.nextafter(9223372036.854776, math.inf)):
            boundary_path = output / f"boundary-{seconds.hex()}.json"
            boundary_result = output / f"boundary-{seconds.hex()}-result.json"
            boundary_path.write_text(json.dumps(self.timed_settings(seconds)), encoding="utf-8")
            boundary = run_engine(
                "solve", "--settings", boundary_path, "--object-report", self.object_report,
                "--result", boundary_result,
            )
            self.assertEqual(boundary.returncode, 2, (boundary.stdout, boundary.stderr))
            self.assertEqual(json.loads(boundary.stdout)["error"]["code"], "INVALID_SETTINGS")
            self.assertFalse(boundary_result.exists())

    @unittest.skipUnless(os.name == "nt", "Ctrl-Break is a Windows console control")
    def test_console_stop(self) -> None:
        output = self.root / "console-stop"
        output.mkdir()
        settings_path = output / "settings.json"
        result_path = output / "result.json"
        settings_path.write_text(json.dumps(self.stopping_settings()), encoding="utf-8")

        synthetic_pid = output / "no-readiness.pid"
        synthetic = subprocess.Popen(
            [sys.executable, __file__, "--console-helper-no-readiness", str(synthetic_pid)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            creationflags=subprocess.CREATE_NEW_CONSOLE,
            startupinfo=hidden_startupinfo(),
        )
        try:
            synthetic_stdout, synthetic_stderr = synthetic.communicate(timeout=10)
        finally:
            if synthetic.poll() is None:
                synthetic.kill()
                synthetic.wait(timeout=5)
        self.assertEqual(synthetic.returncode, 0, (synthetic_stdout, synthetic_stderr))
        self.assertIn("solve readiness timed out", synthetic_stderr)
        self.assertTrue(synthetic_pid.is_file())
        self.assertTrue(process_exited(int(synthetic_pid.read_text(encoding="ascii")), 5000))

        process = subprocess.Popen(
            [sys.executable, __file__, "--console-helper", str(ENGINE), str(settings_path),
             str(self.object_report), str(result_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            creationflags=subprocess.CREATE_NEW_CONSOLE,
            startupinfo=hidden_startupinfo(),
        )
        try:
            stdout, stderr = process.communicate(timeout=15)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

        self.assertEqual(process.returncode, 0, (stdout, stderr))
        response = json.loads(stdout)
        self.assertEqual(response["termination_reason"], "user_stopped")
        result = json.loads(result_path.read_text(encoding="utf-8"))
        readiness = stderr.splitlines()[0] if stderr else ""
        observed_count = validated_readiness_count(readiness)
        # The first validated snapshot may be empty; positive-incumbent Stop is covered by native tests.
        self.assertGreaterEqual(result["count"], observed_count, (stdout, stderr))
        self.assertEqual(result["count"], len(result["placements"]))
        self.assertEqual(response["count"], result["count"])
        self.assertEqual(result["validation"]["status"], "valid")
        self.assertEqual(result["label"], "best_found")
        self.assertEqual(result["metrics"]["termination_reason"], "user_stopped")
        self.assertEqual(response["no_nonempty_incumbent"], result["count"] == 0)


CASES = {
    "solve_deadline": "test_deadline",
    "solve_console_stop": "test_console_stop",
}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", nargs="?")
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--console-helper", nargs=4, metavar=("ENGINE", "SETTINGS", "REPORT", "RESULT"))
    parser.add_argument("--console-helper-no-readiness", metavar="PID_PATH")
    arguments = parser.parse_args()
    if arguments.console_helper:
        helper_engine, helper_settings, helper_report, helper_result = arguments.console_helper
        raise SystemExit(console_helper(pathlib.Path(helper_engine), pathlib.Path(helper_settings),
                                        pathlib.Path(helper_report), pathlib.Path(helper_result)))
    if arguments.console_helper_no_readiness:
        raise SystemExit(no_readiness_helper(pathlib.Path(arguments.console_helper_no_readiness)))
    if not arguments.engine:
        parser.error("engine is required outside helper modes")
    ENGINE = pathlib.Path(arguments.engine)
    suite = (unittest.TestSuite([SolveControls(CASES[arguments.case])])
             if arguments.case else unittest.defaultTestLoader.loadTestsFromTestCase(SolveControls))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
