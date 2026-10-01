"""T009-A6/A7: serial CLI timing or independent reread of native-core export evidence.

The timing mode is a CLI check, not desktop GUI qualification. Journey mode reads
the evidence produced by the real Rust core test and checks every STL vertex.
"""
import argparse
import hashlib
import json
import pathlib
import statistics
import struct
import subprocess
import time

import solve_controls_subprocess_test as controls
import solve_subprocess_test as fixtures


def run(engine, directory, *args, timeout_seconds=1800):
    directory.mkdir(parents=True, exist_ok=False)
    invocation = {"argv": [str(engine), *map(str, args)], "timeout_seconds": timeout_seconds}
    started = time.perf_counter()
    stdout, stderr = b"", b""
    try:
        completed = subprocess.run(invocation["argv"], capture_output=True, timeout=timeout_seconds)
        stdout, stderr = completed.stdout, completed.stderr
        invocation.update(status="completed", returncode=completed.returncode)
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or b"", error.stderr or b""
        invocation.update(status="timeout", returncode=None)
        raise
    except OSError as error:
        invocation.update(status="launch_failed", returncode=None, error=str(error))
        raise
    finally:
        invocation["elapsed_seconds"] = time.perf_counter() - started
        (directory / "stdout.json").write_bytes(stdout)
        (directory / "stderr.log").write_bytes(stderr)
        (directory / "invocation.json").write_text(json.dumps(invocation, indent=2), encoding="utf-8")
    assert completed.returncode == 0, (completed.returncode, directory)
    return invocation["elapsed_seconds"]


def benchmark(engine, output, before=None):
    output.mkdir(parents=True, exist_ok=False)
    source, report = output / "cuboid.stl", output / "object.report.json"
    vertices = [(x, y, z) for x, y, z in fixtures.cube_vertices(extent=1)]
    vertices = [(2*x-1, 3*y-1.5, z-.5) for x, y, z in vertices]
    data = bytearray(80) + struct.pack("<I", len(fixtures.SOURCE_FACES))
    for face in fixtures.SOURCE_FACES:
        data += struct.pack("<3f", 0, 0, 0)
        for index in face:
            data += struct.pack("<3f", *vertices[index])
        data += struct.pack("<H", 0)
    source.write_bytes(data)
    provenance = {"profile": "t009-medium-cli-v1", "engine_sha256": hashlib.sha256(engine.read_bytes()).hexdigest(),
                  "source_sha256": hashlib.sha256(data).hexdigest(), "requested_settings": {
                      "box_mm": [22, 18, 12], "clearance_mm": {"pair": .125, "wall": .25},
                      "pitch_mm": 1, "orientation": "fixed_identity", "seed": "0", "backend": "cpu",
                      "thread_count": 1, "candidate_budget": 2, "pass_budget": 2}}
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    run(engine, output / "inspect", "inspect", "--stl", source, "--units", "mm", "--report", report)
    case = controls.SolveControls()
    case.object_report = report
    settings = case.settings("t009-medium-cli-v1")
    settings["container"]["dimensions_mm"] = [22, 18, 12]
    settings["clearance_mm"] = {"pair": .125, "wall": .25}
    settings["resolution"]["pitch_mm"] = settings["resolved"]["pitch_mm"] = 1
    settings_path = output / "settings.json"
    settings_path.write_text(json.dumps(settings), encoding="utf-8")
    provenance["settings"] = settings
    provenance["settings_sha256"] = hashlib.sha256(settings_path.read_bytes()).hexdigest()
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    samples = []
    for index in range(6):
        directory = output / ("warmup" if index == 0 else f"sample-{index}")
        result = directory / "result.json"
        seconds = run(engine, directory, "solve", "--settings", settings_path,
                      "--object-report", report, "--result", result)
        document = json.loads(result.read_text(encoding="utf-8"))
        assert document["count"] >= 2 and document["validation"]["status"] == "valid"
        samples.append({"seconds": seconds, "count": document["count"],
                        "work": document["search"]["work_counts"], "placements": document["placements"]})
    assert hashlib.sha256(engine.read_bytes()).hexdigest() == provenance["engine_sha256"], "engine changed during samples"
    assert hashlib.sha256(source.read_bytes()).hexdigest() == provenance["source_sha256"], "source changed during samples"
    summary = {"profile": "t009-medium-cli-v1", "engine_sha256": provenance["engine_sha256"],
               "source_sha256": hashlib.sha256(data).hexdigest(), "settings": settings,
               "warmup": samples[0], "samples": samples[1:],
               "median_seconds": statistics.median(s["seconds"] for s in samples[1:])}
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"CLI median {summary['median_seconds']:.9f} s; count {samples[-1]['count']}")
    if before:
        original = json.loads(before.read_text(encoding="utf-8"))
        assert summary["settings"] == original["settings"]
        for old, new in zip(original["samples"], summary["samples"]):
            assert (new["count"], new["work"], new["placements"]) == (old["count"], old["work"], old["placements"])
        assert summary["median_seconds"] <= original["median_seconds"] * .7, "frozen 30% improvement target"


def reread(output):
    document = json.loads((output / "document.json").read_text(encoding="utf-8"))
    source_hash = document["assets"]["object"]["source"]["sha256"]
    manifest = json.loads((pathlib.Path(__file__).parents[1] / "fixtures/rc-manifest.json").read_text())
    source_record = next(item for item in manifest["records"] if item["sha256"] == source_hash)
    expectations = json.loads((pathlib.Path(__file__).parents[1] / "fixtures/import-expectations.json").read_text())
    expected = next(item["expected"] for item in expectations["records"] if item["path"] == source_record["path"])
    assert expected["state"] == "accepted"
    assert document["assets"]["object"]["accepted_solid"]["sha256"] == expected["artifact_sha256"]
    assert document["count"] == len(document["placements"]) > 0
    stl_count = 0
    bundles = list((output / "exports").iterdir())
    assert len(bundles) == 2, "both requested JSON and STL exports must exist"
    checks = fixtures.SolveSubprocessTests()
    for bundle in bundles:
        exported = json.loads((bundle / "result.json").read_text(encoding="utf-8"))
        source = bundle / exported["assets"]["object"]["source"]["path"]
        assert hashlib.sha256(source.read_bytes()).hexdigest() == source_hash
        for key in ("search", "placements", "metrics", "count"):
            assert exported[key] == document[key], key
        accepted = exported["assets"]["object"]["accepted_solid"]
        ply_bytes, vertices, faces = fixtures.parse_ply(bundle / accepted["path"])
        assert accepted["sha256"] == hashlib.sha256(ply_bytes).hexdigest() == expected["artifact_sha256"]
        assert (accepted["vertex_count"], accepted["triangle_count"]) == (len(vertices), len(faces))
        if not (bundle / "packed.stl").exists():
            continue
        stl_count += 1
        artifact, _, triangles, companion = checks.artifact_and_companion(exported, bundle)
        checks.assert_companion(companion, exported, artifact, len(faces))
        assert len(triangles) == len(faces) * document["count"]
        for copy_index, pose in enumerate(document["placements"]):
            matrix = pose["local_to_world"]
            for face_index, face in enumerate(faces):
                actual = triangles[copy_index * len(faces) + face_index][1]
                for actual_vertex, vertex_index in zip(actual, face):
                    vertex = vertices[vertex_index]
                    expected_vertex = tuple(struct.unpack("<f", struct.pack("<f", sum(matrix[row][axis] * vertex[axis]
                                     for axis in range(3)) + matrix[row][3]))[0] for row in range(3))
                    assert actual_vertex == expected_vertex, (copy_index, face_index)
    assert stl_count == 1
    print(f"Reread checked {document['count']} copies, source {source_hash}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("benchmark", "reread"))
    parser.add_argument("--engine", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--before", type=pathlib.Path, help="original timing summary for frozen A7 comparison")
    args = parser.parse_args()
    if args.mode == "benchmark":
        assert args.engine
        benchmark(args.engine.resolve(), args.output.resolve(), args.before)
    else:
        reread(args.output.resolve())
