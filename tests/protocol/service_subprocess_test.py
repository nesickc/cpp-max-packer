"""Executable protocol check for DATA-01; invoked by CTest with the engine path."""
import json
import subprocess
import sys
import tempfile
import queue
import threading

engine = sys.argv[1]
capabilities = subprocess.run([engine, "capabilities", "--json"], capture_output=True, text=True, timeout=5)
assert capabilities.returncode == 0, capabilities.stderr
capability_value = json.loads(capabilities.stdout)

with tempfile.TemporaryDirectory() as outside_checkout:
    process = subprocess.Popen([engine, "serve", "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, cwd=outside_checkout)
    try:
        request = {"protocol_version": 1, "request_id": "subprocess-1", "method": "capabilities.get", "params": {}}
        assert process.stdin is not None and process.stdout is not None
        process.stdin.write(json.dumps(request) + "\n")
        process.stdin.flush()
        responses = queue.Queue()
        reader = threading.Thread(target=lambda: responses.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            response_line = responses.get(timeout=5)
        except queue.Empty:
            raise AssertionError("timed out waiting for service response")
        response = json.loads(response_line)
        assert response == {"protocol_version": 1, "request_id": "subprocess-1", "ok": True, "result": capability_value}
        process.stdin.close()
        assert process.wait(timeout=5) == 0
        assert process.stderr is not None
        assert process.stderr.read() == ""
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process.stdin is not None:
            process.stdin.close()
        if process.stdout is not None:
            process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()

for command in ("inspect", "pack", "validate", "benchmark"):
    unsupported = subprocess.run([engine, command], capture_output=True, text=True, timeout=5)
    assert unsupported.returncode == 3
    assert len(unsupported.stdout.splitlines()) == 1
    assert json.loads(unsupported.stdout)["error"]["code"] == "METHOD_UNSUPPORTED"
    assert unsupported.stderr == ""
for command in (("capabilities", "--bad"), ("unknown",)):
    invalid = subprocess.run([engine, *command], capture_output=True, text=True, timeout=5)
    assert invalid.returncode == 2
    assert json.loads(invalid.stdout)["error"]["code"] == "INVALID_REQUEST"

# Raw bytes deliberately avoid Python text-mode newline and Ctrl-Z translations.
raw = subprocess.Popen([engine, "serve", "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    valid = json.dumps({"protocol_version": 1, "request_id": "raw", "method": "capabilities.get", "params": {}}, separators=(",", ":")).encode()
    padded = valid + b" " * (1048576 - len(valid)) + b"\r\n"
    stdout, stderr = raw.communicate(padded, timeout=5)
    assert b"RECORD_TOO_LARGE" in stdout and stderr == b""
    assert raw.returncode == 2
finally:
    if raw.poll() is None:
        raw.kill(); raw.communicate(timeout=5)

ctrl_z = subprocess.Popen([engine, "serve", "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    stdout, stderr = ctrl_z.communicate(b"\x1a\n" + valid + b"\n", timeout=5)
    lines = stdout.splitlines()
    assert b"INVALID_JSON" in lines[0] and json.loads(lines[1])["request_id"] == "raw"
    assert stderr == b"" and ctrl_z.returncode == 0
finally:
    if ctrl_z.poll() is None:
        ctrl_z.kill(); ctrl_z.communicate(timeout=5)
