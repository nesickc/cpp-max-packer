"""AT-03/AT-04 inspection-command black-box checks."""

import ctypes
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ENGINE = pathlib.Path(sys.argv.pop(1)).resolve()
SOURCE_LIMIT_BYTES = 256 * 1024 * 1024


def run(*args, timeout=15):
    return subprocess.run(
        [str(ENGINE), *map(str, args)], capture_output=True, encoding="utf-8",
        errors="strict", timeout=timeout,
    )


def cube_binary(path, offset=0.0, seam=False):
    vertices = [
        (-1 + offset, -1, -1), (1 + offset, -1, -1),
        (1 + offset, 1, -1), (-1 + offset, 1, -1),
        (-1 + offset, -1, 1), (1 + offset, -1, 1),
        (1 + offset, 1, 1), (-1 + offset, 1, 1),
    ]
    faces = [
        (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
        (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
    ]
    data = bytearray(b"solid deliberately-binary".ljust(80, b"\0"))
    data.extend(struct.pack("<I", len(faces)))
    for face_index, face in enumerate(faces):
        data.extend(struct.pack("<3f", 0, 0, 0))
        for corner, index in enumerate(face):
            point = vertices[index]
            if seam and face_index == 0 and corner == 0:
                point = (point[0] + 0.05, point[1], point[2])
            data.extend(struct.pack("<3f", *point))
        data.extend(struct.pack("<H", 0))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def response_error(completed):
    return json.loads(completed.stdout)["error"]["code"]


class InspectionCommandTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "cube solid.stl"
        self.report = self.root / "output" / "inspection.json"
        cube_binary(self.source)
        self.original = self.source.read_bytes()

    def tearDown(self):
        self.temporary.cleanup()

    def inspect(self, *extra, source=None, report=None, units="mm", timeout=15):
        return run(
            "inspect", "--stl", source or self.source, "--units", units,
            "--report", report or self.report, *extra, timeout=timeout,
        )

    def test_unicode_source_and_report_publish_source_preserving_artifacts(self):
        source = self.root / "куб solid.stl"
        report_path = self.root / "вывод" / "inspection.json"
        cube_binary(source)
        original = source.read_bytes()
        completed = self.inspect(source=source, report=report_path, units="inch")
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        summary = json.loads(completed.stdout)
        value = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(pathlib.Path(summary["report_path"]).resolve(), report_path.resolve())
        self.assertEqual(value["source"]["sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(value["source"]["units"], "inch")
        self.assertEqual(value["dimensions_mm"], [50.8, 50.8, 50.8])
        self.assertEqual(value["frame"]["source_to_local"][0][3], 0.0)
        self.assertEqual(value["diagnostics"]["import"]["encoding"], "binary")

        solid_key = "accepted_solid" if value["state"] == "accepted" else "preview"
        solid = report_path.parent / value[solid_key]["path"]
        blob = solid.read_bytes()
        self.assertTrue(blob.startswith(b"ply\n"))
        self.assertIn(b"property double x", blob)
        self.assertIn(b"property list uchar uint vertex_indices", blob)
        self.assertEqual(hashlib.sha256(blob).hexdigest(), value[solid_key]["sha256"])

    def test_required_and_duplicate_options_are_rejected(self):
        missing = run("inspect", "--stl", self.source, "--report", self.report)
        self.assertEqual(missing.returncode, 2)
        self.assertEqual(response_error(missing), "INVALID_REQUEST")

        duplicate_role = self.inspect("--role", "object", "--role", "object")
        self.assertEqual(duplicate_role.returncode, 2)
        self.assertEqual(response_error(duplicate_role), "INVALID_REQUEST")

        junk_number = self.inspect("--scale-mm", "1junk", units="custom")
        self.assertEqual(junk_number.returncode, 2)
        self.assertEqual(response_error(junk_number), "INVALID_REQUEST")

    def test_report_path_cannot_alias_source_lexically_or_by_hardlink(self):
        lexical = self.inspect(report=self.source)
        self.assertEqual(lexical.returncode, 2)
        self.assertEqual(response_error(lexical), "INVALID_REQUEST")

        linked_report = self.root / "linked-report.json"
        linked_report.hardlink_to(self.source)
        hardlink = self.inspect(report=linked_report)
        self.assertEqual(hardlink.returncode, 2)
        self.assertEqual(response_error(hardlink), "INVALID_REQUEST")
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_staging_name_cannot_destroy_a_source_named_like_the_old_temp(self):
        report = self.root / "source-owner"
        source = pathlib.Path(f"{report}.tmp")
        cube_binary(source)
        original = source.read_bytes()

        completed = self.inspect(source=source, report=report)
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        self.assertEqual(source.read_bytes(), original)
        self.assertTrue(report.is_file())

    def test_conflicting_content_addressed_artifact_preserves_report_and_source(self):
        source_hash = hashlib.sha256(self.original).hexdigest()
        artifact = self.report.parent / "assets" / f"{source_hash}.stl"
        artifact.parent.mkdir(parents=True)
        artifact.write_bytes(b"corrupt")
        self.report.write_text('{"previous":"complete"}', encoding="utf-8")

        completed = self.inspect()
        self.assertEqual(completed.returncode, 4)
        self.assertEqual(response_error(completed), "INTERNAL_ERROR")
        self.assertEqual(artifact.read_bytes(), b"corrupt")
        self.assertEqual(self.report.read_text(encoding="utf-8"), '{"previous":"complete"}')
        self.assertEqual(self.source.read_bytes(), self.original)

    @unittest.skipUnless(os.name == "nt", "Windows replacement-lock behavior")
    def test_failed_atomic_replacement_preserves_previous_report_and_source(self):
        self.report.parent.mkdir(parents=True)
        previous = b'{"previous":"complete"}'
        self.report.write_bytes(previous)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = ctypes.c_void_p
        handle = kernel32.CreateFileW(
            str(self.report), 0x80000000, 0x00000001, None, 3, 0x80, None
        )
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            completed = self.inspect()
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))

        self.assertEqual(completed.returncode, 4)
        self.assertEqual(response_error(completed), "INTERNAL_ERROR")
        self.assertEqual(self.report.read_bytes(), previous)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_source_larger_than_default_cap_is_rejected_without_report(self):
        oversized = self.root / "oversized.stl"
        with oversized.open("wb") as stream:
            stream.truncate(SOURCE_LIMIT_BYTES + 1)

        completed = self.inspect(source=oversized, timeout=5)
        self.assertEqual(completed.returncode, 3)
        self.assertEqual(response_error(completed), "MEMORY_LIMIT")
        self.assertFalse(self.report.exists())

    def test_repair_token_hashes_record_bytes_and_exactly_binds_acceptance(self):
        cube_binary(self.source, seam=True)
        self.original = self.source.read_bytes()
        proposed = self.inspect("--weld-tolerance-mm", "0.1")
        self.assertEqual(proposed.returncode, 0, (proposed.stdout, proposed.stderr))
        summary = json.loads(proposed.stdout)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        token = summary["proposal_sha256"]
        proposal_ref = report["repair_proposal"]
        proposal_path = self.report.parent / proposal_ref["path"]
        proposal_bytes = proposal_path.read_bytes()
        proposal = json.loads(proposal_bytes)

        self.assertEqual(hashlib.sha256(proposal_bytes).hexdigest(), token)
        self.assertEqual(proposal_ref["sha256"], token)
        self.assertEqual(proposal["source"]["sha256"], hashlib.sha256(self.original).hexdigest())
        self.assertIn("options", proposal)
        self.assertEqual(proposal["before"]["diagnostics"]["status"], "invalid")
        self.assertEqual(proposal["before"]["diagnostics"]["messages"], [])
        self.assertGreater(proposal["before"]["diagnostics"]["import"]["boundary_edges"], 0)
        self.assertEqual(proposal["after"]["diagnostics"]["status"], "valid")
        self.assertEqual(proposal["after"]["diagnostics"]["messages"], [])
        self.assertEqual(proposal["after"]["diagnostics"]["import"]["boundary_edges"], 0)
        self.assertGreater(proposal["max_displacement_mm"], 0)
        self.assertEqual(proposal_ref["after"]["sha256"], proposal["after"]["mesh_sha256"])

        previous = self.report.read_bytes()
        mismatch = self.inspect(
            "--weld-tolerance-mm", "0.1", "--accept-repair", "0" * 64
        )
        self.assertEqual(mismatch.returncode, 2)
        self.assertEqual(response_error(mismatch), "ASSET_MISMATCH")
        self.assertEqual(self.report.read_bytes(), previous)
        self.assertEqual(self.source.read_bytes(), self.original)

        accepted = self.inspect(
            "--weld-tolerance-mm", "0.1", "--accept-repair", token
        )
        self.assertEqual(accepted.returncode, 0, (accepted.stdout, accepted.stderr))
        accepted_report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(accepted_report["state"], "accepted")
        self.assertEqual(accepted_report["repair_record"]["sha256"], token)
        self.assertTrue(accepted_report["repair_record"]["accepted_by_user"])
        self.assertEqual(
            accepted_report["accepted_solid"]["sha256"],
            proposal_ref["after"]["sha256"],
        )
        self.assertNotIn("repair_proposal", accepted_report)
        self.assertEqual(self.source.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()
