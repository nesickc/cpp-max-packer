"""Regression coverage for immutable vcpkg source assets (M0/T-002 support)."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH_NAME = "0034c113477f83c28d4380de1ee189c25b1168e6.patch"
EXPECTED_SHA512 = (
    "5c165b50813b0d9937ff0eb4d4a81e2d1e77718ac3b0d02b93931c8eddb4e06e"
    "4fae1822c5cc97a5b01c995916a29d0af03fcbcd8f059cb29cfeb0e2371b15e3"
)


def _sha512(path: Path) -> str:
    return hashlib.sha512(path.read_bytes()).hexdigest()


def _copy_seed_repository(destination: Path) -> tuple[Path, Path]:
    tools = destination / "tools"
    assets = destination / "third_party" / "vcpkg"
    tools.mkdir(parents=True)
    assets.mkdir(parents=True)
    shutil.copy2(ROOT / "tools" / "Prepare-VcpkgAssets.ps1", tools)
    shutil.copy2(ROOT / "third_party" / "vcpkg" / PATCH_NAME, assets)
    return tools / "Prepare-VcpkgAssets.ps1", assets / PATCH_NAME


def _prepare(
    script: Path, vcpkg_root: Path, downloads: Path | None = None, *, environment: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    command = ["pwsh", "-NoProfile", "-File", str(script), "-VcpkgRoot", str(vcpkg_root)]
    if downloads is not None:
        command.extend(["-DownloadsRoot", str(downloads)])
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
        env=environment,
    )


class VcpkgAssetTests(unittest.TestCase):
 def test_patch_seed_recovers_the_locked_original_into_an_isolated_vcpkg_cache(self) -> None:
    """The pinned port can recover the historical upstream patch byte-for-byte."""
    source = ROOT / "third_party" / "vcpkg" / PATCH_NAME
    self.assertTrue(source.is_file(), "the committed json-schema-validator patch seed is required")
    self.assertEqual(_sha512(source), EXPECTED_SHA512)

    with tempfile.TemporaryDirectory() as temporary:
        sandbox = Path(temporary)
        script, copied_source = _copy_seed_repository(sandbox / "repository")
        vcpkg_root = sandbox / "vcpkg"
        downloads = sandbox / "downloads"
        downloads.mkdir()
        target = downloads / PATCH_NAME
        target.write_bytes(b"stale cache content")
        environment = os.environ.copy()
        environment["VCPKG_DOWNLOADS"] = str(downloads)

        completed = _prepare(script, vcpkg_root, environment=environment)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(target.read_bytes(), copied_source.read_bytes())
        self.assertEqual(_sha512(target), EXPECTED_SHA512)

 def test_patch_seed_uses_vcpkg_root_downloads_when_no_downloads_override_exists(self) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        sandbox = Path(temporary)
        script, copied_source = _copy_seed_repository(sandbox / "repository")
        vcpkg_root = sandbox / "vcpkg"
        environment = os.environ.copy()
        environment.pop("VCPKG_DOWNLOADS", None)

        completed = _prepare(script, vcpkg_root, environment=environment)

        target = vcpkg_root / "downloads" / PATCH_NAME
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(target.read_bytes(), copied_source.read_bytes())


 def test_tampered_committed_seed_fails_before_the_cache_changes(self) -> None:
    """A bad repository seed must not overwrite a previously usable download cache."""
    source = ROOT / "third_party" / "vcpkg" / PATCH_NAME
    self.assertTrue(source.is_file(), "the committed json-schema-validator patch seed is required")

    with tempfile.TemporaryDirectory() as temporary:
        sandbox = Path(temporary)
        script, copied_source = _copy_seed_repository(sandbox / "repository")
        copied_source.write_bytes(copied_source.read_bytes() + b"tampered")
        vcpkg_root = sandbox / "vcpkg"
        downloads = sandbox / "downloads"
        downloads.mkdir()
        target = downloads / PATCH_NAME
        original_cache = b"do not mutate this cache entry"
        target.write_bytes(original_cache)

        completed = _prepare(script, vcpkg_root, downloads)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("SHA512", completed.stderr)
        self.assertEqual(target.read_bytes(), original_cache)
