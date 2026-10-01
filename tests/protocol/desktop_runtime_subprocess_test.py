"""ADR0013 / T010-A5 / T011-A1-A5: real retained native process boundary."""
import argparse
import ctypes
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

import solve_subprocess_test as fixtures

ENGINE = None


def qpc():
    ticks, frequency = ctypes.c_int64(), ctypes.c_int64()
    assert ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(ticks))
    assert ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))
    return str(ticks.value), str(frequency.value)


class Session:
    def __init__(self):
        self.child = subprocess.Popen([str(ENGINE), "desktop-session"], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True, encoding="utf-8", bufsize=1)
        self.records = queue.Queue()
        self.stderr = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.errors = threading.Thread(target=lambda: self.stderr.append(self.child.stderr.read()), daemon=True)
        self.errors.start()

    def _read(self):
        for line in self.child.stdout:
            self.records.put(json.loads(line))
        self.records.put(None)

    def request(self, value):
        self.child.stdin.write(json.dumps(value) + "\n")
        self.child.stdin.flush()
        phases = []
        while True:
            record = self.records.get(timeout=15)
            assert record is not None, "Native session ended: " + "".join(self.stderr)
            assert record["request_id"] == value["request_id"]
            if record["kind"] == "phase":
                assert record["operation_id"] == value["operation_id"]
                assert not phases or record["sequence"] > phases[-1]["sequence"]
                phases.append(record)
            else:
                return record, phases

    def close(self):
        if self.child.poll() is None:
            self.child.stdin.close()
            try:
                self.child.wait(timeout=6)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait()
                raise
        self.reader.join(timeout=1)
        self.errors.join(timeout=1)
        self.child.stdout.close()
        self.child.stderr.close()


class RuntimeTests(unittest.TestCase):
    def test_versioned_session_shutdown(self):
        session = Session()
        try:
            reply, _ = session.request({"runtime_version": 1, "request_id": "shutdown-1",
                                        "method": "shutdown", "params": {}})
            self.assertTrue(reply["ok"], reply)
            self.assertEqual(session.child.wait(timeout=5), 0)
        finally:
            session.close()

    def test_pinned_asset_reuse_stop_and_deadline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, report = root / "cube.stl", root / "object.report.json"
            fixtures.write_cube_stl(source)
            imported = subprocess.run([str(ENGINE), "inspect", "--stl", str(source), "--units", "mm",
                                       "--report", str(report)], capture_output=True, text=True, timeout=15)
            self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
            session = Session()
            try:
                preview = root / "preview"
                preview.mkdir()
                prepare = {"runtime_version": 1, "request_id": "prepare-1", "operation_id": "prepare-op",
                           "method": "prepare", "params": {"object_report": str(report),
                                                             "output_directory": str(preview)}}
                prepared, _ = session.request(prepare)
                self.assertTrue(prepared["ok"], prepared)
                token = prepared["result"]["asset_token"]
                initial_preview = (preview / "preview.ply").read_bytes()
                # A token pins independently verified source/accepted bytes; changing disk assertions cannot mint authority.
                source.write_bytes(b"tampered")
                report.write_text("{}")
                settings = {"desktop_version": 1, "box_dimensions_mm": [20, 20, 20],
                            "clearance_mm": {"pair": 0, "wall": 0},
                            "orientation": {"mode": "fixed", "quaternion_xyzw": [0, 0, 0, 1]},
                            "pitch_mm": 10, "budget_seconds": .01, "seed": "42",
                            "thread_count": 1, "budget_scope": "total_start"}
                for index, stopped in enumerate((True, False)):
                    output = root / f"run-{index}"
                    output.mkdir()
                    marker = output / "stop.marker"
                    if stopped:
                        marker.write_bytes(b"stop")
                    ticks, frequency = qpc()
                    request = {"runtime_version": 1, "request_id": f"run-{index}",
                               "operation_id": f"run-op-{index}", "method": "run", "params": {
                                   "asset_token": token, "settings": settings, "result_path": str(output / "result.json"),
                                   "stop_file": str(marker), "start_qpc_ticks": ticks, "qpc_frequency_hz": frequency}}
                    reply, phases = session.request(request)
                    self.assertTrue(reply["ok"], reply)
                    self.assertTrue(reply["result"]["preparation_reused"])
                    self.assertEqual(reply["result"]["termination_reason"], "user_stopped" if stopped else "budget_exhausted")
                    self.assertEqual((preview / "preview.ply").read_bytes(), initial_preview)
                    if stopped:
                        self.assertFalse((output / "result.json").exists())
                        self.assertTrue(reply["result"]["no_nonempty_incumbent"])
                    else:
                        self.assertTrue(phases)
                    replay, replay_phases = session.request(request)
                    self.assertEqual(replay, reply)
                    self.assertFalse(replay_phases)
                    request["params"]["settings"]["seed"] = "43"
                    conflicting, _ = session.request(request)
                    self.assertFalse(conflicting["ok"])
                    self.assertEqual(conflicting["error"]["code"], "REQUEST_ID_CONFLICT")
                    settings["seed"] = "42"
            finally:
                session.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=pathlib.Path)
    parser.add_argument("--case", default=None)
    arguments = parser.parse_args()
    ENGINE = arguments.engine.resolve()
    unittest.main(argv=[sys.argv[0]] + (["RuntimeTests." + arguments.case] if arguments.case else []))
