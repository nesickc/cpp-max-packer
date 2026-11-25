"""T-008 / AT-13, AT-14, AT-15: real native desktop boundary checks."""

import argparse
import copy
import json
import pathlib
import math
import struct
import subprocess
import sys
import tempfile
import time
import unittest

import solve_subprocess_test as fixtures
import t009_practical_journey as qualification

ENGINE = None


def run(*arguments):
    # A Debug engine needs 10 to 13 s to build or replay the sphere's packing envelope.
    return subprocess.run([str(ENGINE), *map(str, arguments)], capture_output=True,
                          text=True, encoding="utf-8", timeout=60)


class DesktopTests(unittest.TestCase):
    def test_t015_p2_prepare_admits_asset_under_the_user_ceiling(self):
        self.request["host_memory_limit_bytes"] = "1"
        completed, output = self.prepare()
        self.assertNotEqual(completed.returncode, 0, completed.stdout)
        self.assertFalse((output / "settings.json").exists())

    def test_new_start_keeps_the_lower_user_ceiling(self):
        """SOL-05: a Start's resolved settings keep the user's memory ceiling for the search."""
        self.request["budget_scope"] = "total_start"
        self.request["host_memory_limit_bytes"] = str(128 << 20)
        completed, _ = self.prepare()
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        response = json.loads(completed.stdout)["result"]
        settings = json.loads(pathlib.Path(response["settings_path"]).read_text(encoding="utf-8"))
        self.assertEqual(settings["compute"]["host_memory_limit_bytes"], str(128 << 20))
        self.assertNotIn("resource_policy", settings["resolved"])

    def test_t009_failed_and_timed_out_invocations_keep_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            with self.assertRaises(AssertionError):
                qualification.run(sys.executable, root / "failed", "-c",
                                  "import sys;print('failure');sys.stderr.write('cause');sys.exit(7)")
            failed = json.loads((root / "failed/invocation.json").read_text())
            self.assertEqual((failed["status"], failed["returncode"]), ("completed", 7))
            self.assertEqual((root / "failed/stderr.log").read_bytes(), b"cause")
            with self.assertRaises(subprocess.TimeoutExpired):
                qualification.run(sys.executable, root / "timeout", "-c",
                                  "import time;print('started',flush=True);time.sleep(10)", timeout_seconds=.5)
            timed_out = json.loads((root / "timeout/invocation.json").read_text())
            self.assertEqual((timed_out["status"], timed_out["returncode"]), ("timeout", None))
            self.assertGreater(timed_out["elapsed_seconds"], 0)
            self.assertIn(b"started", (root / "timeout/stdout.json").read_bytes())
            with self.assertRaises(FileExistsError):
                qualification.benchmark(pathlib.Path(sys.executable), root)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="desktop-")
        self.root = pathlib.Path(self.temp.name) / "перенос with spaces"
        self.root.mkdir()
        self.source = self.root / "object.stl"
        fixtures.write_cube_stl(self.source, low=(17.0, -21.0, 33.0))
        self.report = self.root / "object.report.json"
        reply = run("inspect", "--stl", self.source, "--units", "mm", "--report", self.report)
        self.assertEqual(reply.returncode, 0, reply.stdout + reply.stderr)
        self.request = {
            "desktop_version": 1, "box_dimensions_mm": [40.0, 40.0, 40.0],
            "clearance_mm": {"pair": 0.0, "wall": 0.0},
            "orientation": {"mode": "fixed", "quaternion_xyzw": [0, 0, 0, 1]},
            "pitch_mm": 10.0, "budget_seconds": 0.2, "seed": "42",
        }

    def tearDown(self):
        self.temp.cleanup()

    def prepare(self, request=None, directory="prepare"):
        request_path = self.root / "request.json"
        request_path.write_text(json.dumps(request or self.request), encoding="utf-8")
        output = self.root / directory
        output.mkdir()
        return run("desktop-prepare", "--object-report", self.report,
                   "--request", request_path, "--output", output), output

    def prepared(self):
        completed, output = self.prepare()
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        reply = json.loads(completed.stdout)["result"]
        return pathlib.Path(reply["settings_path"]), output

    def solved(self, allow_retained_resource=False):
        settings, _ = self.prepared()
        output = self.root / "solve"
        output.mkdir()
        result = output / "result.json"
        completed = run("solve", "--object-report", self.report, "--settings", settings, "--result", result)
        if completed.returncode != 0 and allow_retained_resource:
            terminal = json.loads(completed.stdout)
            self.assertFalse(terminal["ok"])
            self.assertEqual(terminal["error"]["code"], "RESOURCE_LIMIT")
            self.assertEqual(pathlib.Path(terminal["error"]["details"]["result_path"]), result)
            retained = json.loads(result.read_text(encoding="utf-8"))
            self.assertEqual(retained["validation"]["status"], "valid")
            self.assertGreater(retained["count"], 0)
            self.assertEqual(retained["metrics"]["termination_reason"], "resource_limit")
            return result, retained
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        return result, json.loads(result.read_text(encoding="utf-8"))

    def restore(self, result, directory="restore", stl=False):
        output = self.root / directory
        output.mkdir()
        arguments = ["desktop-restore", "--object-report", self.report,
                     "--result", result, "--output", output]
        if stl:
            arguments += ["--stl", output / "packed.stl"]
        return run(*arguments), output

    def test_prepare_accepted_origin_and_shared_preview(self):
        completed, output = self.prepare()
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        reply = json.loads(completed.stdout)
        self.assertEqual(set(reply), {"ok", "result"})
        self.assertTrue(reply["ok"])
        data = reply["result"]
        settings = json.loads(pathlib.Path(data["settings_path"]).read_text(encoding="utf-8"))
        self.assertEqual(settings["container"]["dimensions_mm"], [40, 40, 40])
        self.assertEqual(settings["resolved"]["orientation_catalog_sha256"], fixtures.IDENTITY_CATALOG_SHA256)
        self.assertEqual(settings["search"]["budget_seconds"], 0.2)
        self.assertEqual(settings["compute"], {"backend": "cpu", "thread_count": 1})
        self.assertEqual(settings["resolved"]["thread_count"], 1)
        preview_bytes, vertices, faces = fixtures.parse_ply(pathlib.Path(data["preview_path"]))
        self.assertEqual(data["preview"]["sha256"], fixtures.sha256_bytes(preview_bytes))
        self.assertEqual(data["preview"]["coordinate_frame"], "object_local_mm")
        self.assertEqual(data["preview"]["triangle_count"], len(faces))
        self.assertEqual(data["preview"]["byte_length"], len(preview_bytes))
        self.assertEqual(tuple(min(v[i] for v in vertices) for i in range(3)), (-5, -5, -5))
        self.assertEqual(tuple(max(v[i] for v in vertices) for i in range(3)), (5, 5, 5))

    def test_prepare_cube_and_bad_settings(self):
        request = copy.deepcopy(self.request)
        request["orientation"] = {"mode": "cube"}
        completed, _ = self.prepare(request, "cube")
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        settings = json.loads(pathlib.Path(json.loads(completed.stdout)["result"]["settings_path"]).read_text())
        self.assertEqual(settings["orientation"], {"mode": "cube"})
        request["box_dimensions_mm"][0] = -1
        completed, output = self.prepare(request, "invalid")
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(json.loads(completed.stdout)["ok"])
        self.assertEqual(list(output.iterdir()), [])

    def test_cube45_diagonal_fit_restores_and_exports(self):
        # A 20 x 2 x 2 bar cannot fit cardinally in 16 x 16 x 4 mm.
        vertices = [(x, y, z) for x, y, z in
                    [(-10,-1,-1),(10,-1,-1),(10,1,-1),(-10,1,-1),
                     (-10,-1,1),(10,-1,1),(10,1,1),(-10,1,1)]]
        payload = bytearray(80) + struct.pack('<I', len(fixtures.SOURCE_FACES))
        for face in fixtures.SOURCE_FACES:
            payload.extend(struct.pack('<12fH', 0, 0, 0,
                                       *(v for index in face for v in vertices[index]), 0))
        self.source.write_bytes(payload)
        imported = run('inspect', '--stl', self.source, '--units', 'mm', '--report', self.report)
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        self.request.update(box_dimensions_mm=[16,16,4], pitch_mm=1, budget_seconds=2,
                            clearance_mm={'pair':0.1,'wall':0.1}, orientation={'mode':'cube45'})
        cardinal = copy.deepcopy(self.request)
        cardinal['orientation'] = {'mode': 'cube'}
        prepared, directory = self.prepare(cardinal, 'cardinal')
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
        cardinal_output = self.root / 'cardinal-solve'
        cardinal_output.mkdir()
        cardinal_result = cardinal_output / 'result.json'
        solved = run('solve', '--object-report', self.report, '--settings', directory / 'settings.json',
                     '--result', cardinal_result)
        self.assertEqual(solved.returncode, 0, solved.stdout + solved.stderr)
        self.assertEqual(json.loads(cardinal_result.read_text())['count'], 0)
        result_path, result = self.solved()
        self.assertGreaterEqual(result['count'], 1)
        self.assertEqual(result['validation']['status'], 'valid')
        resolved = result['search']['resolved_settings']
        self.assertEqual(resolved['orientation']['mode'], 'custom')
        self.assertEqual(len(resolved['orientation']['quaternions_xyzw']), 96)
        restored, output = self.restore(result_path, stl=True)
        self.assertEqual(restored.returncode, 0, restored.stdout + restored.stderr)
        self.assertTrue((output / 'packed.stl').is_file())
        tampered = copy.deepcopy(result)
        tampered['search']['resolved_settings']['orientation']['quaternions_xyzw'].pop()
        broken = self.root / 'tampered.json'
        broken.write_text(json.dumps(tampered))
        rejected, _ = self.restore(broken, 'tampered-restore')
        self.assertNotEqual(rejected.returncode, 0)

    def test_t010_parallel_policy_executes_and_restores_without_rewriting_provenance(self):
        self.request["thread_count"] = 2
        self.request["budget_seconds"] = 1.0
        result_path, result = self.solved()
        resolved = result["search"]["resolved_settings"]["resolved"]
        self.assertEqual(resolved["thread_count"], 2)
        self.assertEqual(resolved["cpu_runtime"]["scheduling_policy"], "raster-rows256-fft-lines64-v1")
        self.assertEqual(result["validation"]["status"], "valid")
        completed, _ = self.restore(result_path)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        restored_path = pathlib.Path(json.loads(completed.stdout)["result"]["result_path"])
        restored = json.loads(restored_path.read_text(encoding="utf-8"))
        self.assertEqual(restored["search"], result["search"])
        self.assertEqual(restored["placements"], result["placements"])

        # Stored execution history remains portable, including the older policy.
        result["search"]["resolved_settings"]["resolved"]["cpu_runtime"]["scheduling_policy"] = "raster-rows256-v1"
        for segment in result["search"]["run_segments"]:
            segment["resolved_settings"]["resolved"]["cpu_runtime"]["scheduling_policy"] = "raster-rows256-v1"
        result_path.write_text(json.dumps(result), encoding="utf-8")
        completed, output = self.restore(result_path, "restore-legacy", stl=True)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        restored_path = pathlib.Path(json.loads(completed.stdout)["result"]["result_path"])
        legacy = json.loads(restored_path.read_text(encoding="utf-8"))
        self.assertEqual(legacy["search"], result["search"])
        self.assertEqual(legacy["placements"], result["placements"])

    def test_t010_stale_explicit_cpu_policy_requires_current_resolution(self):
        self.request["thread_count"] = 2
        settings_path, _ = self.prepared()
        settings = json.loads(settings_path.read_text(encoding="utf-8"))
        settings["resolved"]["cpu_runtime"]["scheduling_policy"] = "raster-rows256-v1"
        settings_path.write_text(json.dumps(settings), encoding="utf-8")
        output = self.root / "stale-solve"
        output.mkdir()
        result_path = output / "result.json"
        completed = run("solve", "--object-report", self.report, "--settings", settings_path, "--result", result_path)
        self.assertNotEqual(completed.returncode, 0)
        error = json.loads(completed.stdout)["error"]
        self.assertEqual(error["code"], "CPU_RUNTIME_UNSUPPORTED")
        self.assertEqual(error["details"]["current_policy"], "raster-rows256-fft-lines64-v1")
        self.assertIn("desktop-prepare", error["details"]["capability_guidance"])
        self.assertFalse(result_path.exists())

    def test_restore_without_search_preserves_history_and_checked_export(self):
        result_path, result = self.solved()
        completed, output = self.restore(result_path, stl=True)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        reply = json.loads(completed.stdout)["result"]
        restored = json.loads(pathlib.Path(reply["result_path"]).read_text(encoding="utf-8"))
        for field in ("job_id", "solution_revision", "created_at", "engine", "search", "placements", "metrics"):
            self.assertEqual(restored[field], result[field], field)
        self.assertEqual(reply["validation_status"], "valid")
        self.assertEqual(restored["validation"]["status"], "valid")
        stl_bytes, triangles = fixtures.parse_stl(pathlib.Path(reply["stl_path"]))
        self.assertEqual(len(triangles), result["count"] * 12)
        self.assertTrue(pathlib.Path(reply["companion_path"]).is_file())

    def test_restore_rejects_tampered_metrics_matrix_and_geometry(self):
        result_path, result = self.solved()
        self.assertGreater(result["count"], 0)
        for mutation in ("metrics", "matrix", "geometry", "schema", "duplicate"):
            changed = copy.deepcopy(result)
            if mutation == "metrics":
                changed["metrics"]["solid_volume_mm3"] *= 2
            elif mutation == "matrix":
                changed["placements"][0]["local_to_world"][0][3] += 1
            elif mutation == "geometry":
                changed["placements"][0]["translation_mm"][0] = 1000
                changed["placements"][0]["local_to_world"][0][3] = 1000
            elif mutation == "schema":
                changed["schema_version"] = 3
            tampered = self.root / (mutation + ".json")
            payload = json.dumps(changed)
            if mutation == "duplicate":
                payload = '{"count":999,' + payload[1:]
            tampered.write_text(payload, encoding="utf-8")
            completed, output = self.restore(tampered, mutation)
            self.assertNotEqual(completed.returncode, 0, mutation)
            self.assertFalse(json.loads(completed.stdout)["ok"])
            self.assertEqual(list(output.iterdir()), [], mutation)

    def test_stop_file_preexisting_preserves_previous_result_without_new_authority(self):
        settings, _ = self.prepared()
        result = self.root / "stopped" / "result.json"
        result.parent.mkdir()
        completed = run("solve", "--object-report", self.report, "--settings", settings,
                        "--result", result)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        previous = result.read_bytes()
        document = json.loads(previous)
        self.assertEqual(document["validation"]["status"], "valid")
        self.assertGreater(document["count"], 0)
        marker = self.root / "stop.marker"
        marker.touch()
        fresh = result.parent / "no-new-result.json"
        for destination in (result, fresh):
            completed = run("solve", "--object-report", self.report, "--settings", settings,
                            "--result", destination, "--stop-file", marker)
            self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
            failure = json.loads(completed.stdout)["error"]
            self.assertEqual(failure["code"], "OPERATION_CANCELLED")
            self.assertTrue(failure["details"]["no_nonempty_incumbent"])
            self.assertTrue(failure["details"]["runtime"]["no_nonempty_incumbent"])
            self.assertEqual(result.read_bytes(), previous)
            self.assertFalse(fresh.exists())

    def test_export_progress_changes_no_output_byte_and_a_stop_marker_writes_nothing(self):
        """UI-03/AT-16: --progress adds stage lines before the terminal record and changes
        no output byte; a stop marker present at the start cancels before any output."""
        result_path, result = self.solved()
        plain, plain_output = self.restore(result_path, "plain", stl=True)
        self.assertEqual(plain.returncode, 0, plain.stdout + plain.stderr)
        self.assertEqual(len(plain.stdout.splitlines()), 1)
        marker = self.root / "stop.marker"
        output = self.root / "progress"
        output.mkdir()
        reported = run("desktop-restore", "--object-report", self.report, "--result", result_path,
                       "--output", output, "--stl", output / "packed.stl", "--progress", "--stop-file", marker)
        self.assertEqual(reported.returncode, 0, reported.stdout + reported.stderr)
        *lines, terminal = reported.stdout.splitlines()
        self.assertEqual(json.loads(terminal)["result"]["count"], result["count"])
        progress = [json.loads(line)["progress"] for line in lines]
        written = [record for record in progress if record["stage"] == "Writing the STL"]
        self.assertEqual(written[0]["total"], result["count"])
        for record in progress:
            self.assertEqual("done" in record, "total" in record, record)
            self.assertLessEqual(record.get("done", 0), record.get("total", 0), record)
        for name in ("result.json", "packed.stl", "packed.stl.json"):
            self.assertEqual((output / name).read_bytes(), (plain_output / name).read_bytes(), name)
        marker.touch()
        stopped = self.root / "stopped"
        stopped.mkdir()
        cancelled = run("desktop-restore", "--object-report", self.report, "--result", result_path,
                        "--output", stopped, "--stl", stopped / "packed.stl", "--stop-file", marker)
        self.assertEqual(cancelled.returncode, 3, cancelled.stdout)
        self.assertEqual(json.loads(cancelled.stdout)["error"]["code"], "OPERATION_CANCELLED")
        self.assertEqual(list(stopped.iterdir()), [])

    def test_stop_during_a_checked_envelope_export_leaves_no_stl(self):
        """AT-16: the stop marker ends a running export, while it rebuilds the envelope
        or checks copies, before it completes and without a packed STL or a stage file."""
        fixtures.write_sphere_stl(self.source)
        allowance = ("--available-host-bytes", str(2 << 30))
        imported = run("inspect", "--stl", self.source, "--units", "mm", "--report", self.report,
                       "--packing-envelope", "auto", "--accept-repair", "auto", *allowance)
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        self.request.update(box_dimensions_mm=[130, 130, 90], pitch_mm=5, budget_seconds=2)
        result_path, result = self.solved()
        self.assertGreater(result["count"], 4)
        for stage in ("Wrapping the packing envelope", "Checking the exported copies"):
            with self.subTest(stage=stage):
                name = stage.split()[0]
                output, marker = self.root / name, self.root / f"{name}.stop"
                output.mkdir()
                process = subprocess.Popen(
                    [str(ENGINE), "desktop-restore", "--object-report", str(self.report), "--result",
                     str(result_path), "--output", str(output), "--stl", str(output / "packed.stl"), *allowance,
                     "--progress", "--stop-file", str(marker)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
                stopped = False
                try:
                    for line in process.stdout:
                        if stage in line:
                            marker.touch()
                            stopped = True
                            break
                    # The engine stops at its next check: within half a second in Release,
                    # after the copies being checked in Debug.
                    rest, _ = process.communicate(timeout=60)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                self.assertTrue(stopped, rest)
                self.assertEqual(process.returncode, 3, rest)
                self.assertEqual(json.loads(rest.splitlines()[-1])["error"]["code"], "OPERATION_CANCELLED")
                self.assertFalse((output / "packed.stl").exists())
                self.assertEqual([path for path in output.rglob("*") if ".stage-" in path.name], [])

    def test_restore_stl_quantization_failure_retains_valid_json_and_source(self):
        original_path, _ = self.solved()
        bound = self.root / "fixed-poses.json"
        source = self.source.read_bytes()
        fixture_builder = ENGINE.parent / "spectrapack_result_contract_check.exe"
        made = subprocess.run([str(fixture_builder), "--quantization-fixture", str(self.report),
            str(original_path), str(bound)], capture_output=True, text=True, encoding="utf-8", timeout=25)
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        completed, output = self.restore(bound, "quantization-failure", stl=True)
        self.assertNotEqual(completed.returncode, 0, completed.stdout)
        failure = json.loads(completed.stdout)["error"]
        self.assertEqual(failure["code"], "EXPORT_QUANTIZATION_FAILED")
        retained = json.loads((output / "result.json").read_text())
        self.assertEqual(retained["validation"]["status"], "valid")
        self.assertEqual(retained["placements"], json.loads(bound.read_text())["placements"])
        self.assertNotIn("artifacts", retained)
        self.assertFalse((output / "packed.stl").exists())
        self.assertEqual(self.source.read_bytes(), source)

    def test_stop_file_invalid_parent_is_operational_failure(self):
        settings, _ = self.prepared()
        output = self.root / "bad-stop"
        output.mkdir()
        completed = run("solve", "--object-report", self.report, "--settings", settings,
                        "--result", output / "result.json", "--stop-file", self.source / "marker")
        self.assertNotEqual(completed.returncode, 0, completed.stdout)
        self.assertEqual(json.loads(completed.stdout)["error"]["code"], "STOP_MONITOR_FAILED")
        self.assertFalse((output / "result.json").exists())
        self.assertEqual(list(output.iterdir()), [])

    def test_stop_file_parent_disappearing_midrun_retains_operational_failure(self):
        self.request.update(box_dimensions_mm=[20, 20, 20], pitch_mm=1, budget_seconds=30)
        settings, _ = self.prepared()
        marker_parent = self.root / "marker-parent"
        marker_parent.mkdir()
        output = self.root / "monitor-runtime"
        output.mkdir()
        with (self.root / "progress.log").open("w", encoding="utf-8") as progress:
            process = subprocess.Popen([str(ENGINE), "solve", "--object-report", str(self.report),
                "--settings", str(settings), "--result", str(output / "result.json"),
                "--stop-file", str(marker_parent / "marker")], stdout=subprocess.PIPE,
                stderr=progress, text=True, encoding="utf-8")
            try:
                deadline = time.monotonic() + 15
                while "solve: validated count=" not in (self.root / "progress.log").read_text():
                    self.assertIsNone(process.poll(), "solver must be active before marker parent removal")
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.02)
                marker_parent.rmdir()
                terminal, _ = process.communicate(timeout=15)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        self.assertNotEqual(process.returncode, 0, terminal)
        self.assertEqual(json.loads(terminal)["error"]["code"], "STOP_MONITOR_FAILED")
        retained = json.loads((output / "result.json").read_text())
        self.assertEqual(retained["validation"]["status"], "valid")
        self.assertEqual(retained["metrics"]["termination_reason"], "error")

    def test_repaired_provenance_replay_restore_and_tampering(self):
        data = bytearray(self.source.read_bytes())
        point = struct.unpack_from("<3f", data, 96)
        struct.pack_into("<3f", data, 96, point[0] + 0.05, point[1], point[2])
        self.source.write_bytes(data)
        proposal = run("inspect", "--stl", self.source, "--units", "mm", "--report", self.report,
                       "--weld-tolerance-mm", "0.1")
        self.assertEqual(proposal.returncode, 0, proposal.stdout)
        token = json.loads(proposal.stdout)["proposal_sha256"]
        accepted = run("inspect", "--stl", self.source, "--units", "mm", "--report", self.report,
                       "--weld-tolerance-mm", "0.1", "--accept-repair", token)
        self.assertEqual(accepted.returncode, 0, accepted.stdout)
        self.request["pitch_mm"] = 10.0
        self.request["box_dimensions_mm"] = [30, 30, 30]
        result_path, result = self.solved(allow_retained_resource=True)
        completed, output = self.restore(result_path, stl=True)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        restored = json.loads((output / "result.json").read_text())
        report = restored["assets"]["object"]
        (output / "object.report.json").write_text(json.dumps(report), encoding="utf-8")
        second_output = self.root / "relocated-repair"
        second_output.mkdir()
        relocated = run("desktop-restore", "--object-report", output / "object.report.json",
                        "--result", output / "result.json", "--output", second_output)
        self.assertEqual(relocated.returncode, 0, relocated.stdout + relocated.stderr)
        original = json.loads(self.report.read_text())
        recipe = self.report.parent / original["repair_record"]["path"]
        recipe_bytes = recipe.read_bytes()
        for mutation in ("token", "recipe", "rebound_recipe", "acceptance", "candidate", "source"):
            paths = []
            changed = copy.deepcopy(original)
            if mutation == "token":
                changed["repair_record"]["sha256"] = "0" * 64
                self.report.write_text(json.dumps(changed), encoding="utf-8")
            elif mutation in ("recipe", "rebound_recipe"):
                value = json.loads(recipe_bytes)
                value["options"]["weld_tolerance_mm"] = 0.000001
                encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
                if mutation == "recipe":
                    recipe.write_bytes(encoded)
                    paths.append((recipe, recipe_bytes))
                else:
                    hashed = fixtures.sha256_bytes(encoded)
                    replacement = self.report.parent / "assets" / (hashed + ".json")
                    replacement.write_bytes(encoded)
                    changed["repair_record"].update(path="assets/" + hashed + ".json", sha256=hashed)
                    self.report.write_text(json.dumps(changed), encoding="utf-8")
            elif mutation == "acceptance":
                changed["repair_record"]["accepted"] = False
                self.report.write_text(json.dumps(changed), encoding="utf-8")
            else:
                key = "accepted_solid" if mutation == "candidate" else "source"
                artifact = self.report.parent / original[key]["path"]
                data = artifact.read_bytes()
                artifact.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
                paths.append((artifact, data))
            completed, _ = self.prepare(directory="bad-repair-" + mutation)
            self.assertNotEqual(completed.returncode, 0, mutation)
            for path, data in paths:
                path.write_bytes(data)
            self.report.write_text(json.dumps(original), encoding="utf-8")

    def test_asymmetric_inches_quarter_turn_preserves_source_and_export_vertices(self):
        data = bytearray(self.source.read_bytes())
        for triangle in range(12):
            for corner in range(3):
                offset = 84 + triangle * 50 + 12 + corner * 12
                x, y, z = struct.unpack_from("<3f", data, offset)
                struct.pack_into("<3f", data, offset, x / 10, y / 5, z * 0.3)
        self.source.write_bytes(data)
        inspected = run("inspect", "--stl", self.source, "--units", "inch", "--report", self.report)
        self.assertEqual(inspected.returncode, 0, inspected.stdout)
        self.request["box_dimensions_mm"] = [53, 28, 80]
        self.request["clearance_mm"] = {"pair": 0, "wall": 1}
        self.request["pitch_mm"] = 5
        self.request["orientation"]["quaternion_xyzw"] = [0, 0, math.sqrt(0.5), math.sqrt(0.5)]
        result_path, result = self.solved()
        self.assertGreater(result["count"], 0)
        completed, output = self.restore(result_path, stl=True)
        self.assertEqual(completed.returncode, 0, completed.stdout)
        _, triangles = fixtures.parse_stl(output / "packed.stl")
        _, vertices, faces = fixtures.parse_ply(output / result["assets"]["object"]["accepted_solid"]["path"])
        pose = result["placements"][0]
        # A +90-degree Z turn maps (x,y,z) to (-y,x,z), independently of Three/native matrices.
        for corner, index in enumerate(faces[0]):
            x, y, z = vertices[index]
            t = pose["translation_mm"]
            expected = (-y + t[0], x + t[1], z + t[2])
            for actual, target in zip(triangles[0][1][corner], expected):
                self.assertAlmostEqual(actual, target, places=4)
        source_path = output / result["assets"]["object"]["source"]["path"]
        self.assertEqual(source_path.read_bytes(), bytes(data))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=pathlib.Path)
    parser.add_argument("--case")
    args = parser.parse_args()
    ENGINE = args.engine.resolve()
    unittest.main(argv=[__file__, args.case] if args.case else [__file__], verbosity=2)
