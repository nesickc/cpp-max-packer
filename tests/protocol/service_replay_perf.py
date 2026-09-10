"""Informational fixed-work DATA-01 service/replay timing; never an acceptance threshold."""
import argparse
import json
import subprocess
import time
import platform
import sys

parser = argparse.ArgumentParser()
parser.add_argument("engine")
parser.add_argument("--samples", type=int, default=5)
parser.add_argument("--requests", type=int, default=1000)
args = parser.parse_args()
if args.samples <= 0 or args.samples > 100 or args.requests <= 0 or args.requests > 100000:
    parser.error("samples must be 1..100 and requests must be 1..100000")

request_value = {"protocol_version": 1, "request_id": "replay-perf", "method": "capabilities.get", "params": {}}
request = json.dumps(request_value, separators=(",", ":")) + "\n"

capabilities = subprocess.run([args.engine, "capabilities", "--json"], capture_output=True, text=True, timeout=10)
assert capabilities.returncode == 0, capabilities.stderr
capability_value = json.loads(capabilities.stdout)
expected_response = json.dumps({"protocol_version": 1, "request_id": "replay-perf", "ok": True,
                                "result": capability_value}, separators=(",", ":"), sort_keys=True)

def sample() -> float:
    process = subprocess.Popen([args.engine, "serve", "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        start = time.perf_counter()
        stdout, stderr = process.communicate(request * args.requests, timeout=10)
        elapsed_ms = (time.perf_counter() - start) * 1000
        assert process.returncode == 0, stderr
        responses = stdout.splitlines()
        assert len(responses) == args.requests
        assert all(response == expected_response for response in responses)
        assert stderr == ""
        return elapsed_ms
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

# One warm-up is deliberately excluded from the reported five fixed-work samples.
sample()
measurements_ms = [sample() for _ in range(args.samples)]
print(json.dumps({"mode": "informational", "requests_per_sample": args.requests,
                  "warmup_samples": 1, "samples": args.samples, "samples_ms": measurements_ms,
                  "engine": str(__import__('pathlib').Path(args.engine).resolve()),
                  "engine_metadata": capability_value["engine"],
                  "environment": {"platform": platform.platform(), "python": sys.version},
                  "config": {"timeout_seconds": 10}}, separators=(",", ":")))
