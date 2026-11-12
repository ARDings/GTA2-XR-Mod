#!/usr/bin/env python3
"""Importer regression tests using generated data only, in isolated folders under the work root."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import gta2ceiling_importer as importer
from prepare_private_container import prepare_container

FIXTURE_EXE = b"GTA2Ceiling synthetic EXE fixture, not game code."


class TestImporter(unittest.TestCase):
    def setUp(self):
        self.test_parent = importer.resolved_work_path(importer.work_root() / "test-artifacts")
        self.test_parent.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="importer-", dir=self.test_parent))
        self.source = self.work / "source"
        self.output = self.work / "output"
        self.source.mkdir()
        for name in importer.MANDATORY_FILES:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(FIXTURE_EXE if name == "gta2.exe" else f"fixture:{name}".encode())
        self.hash_patch = patch.object(importer, "EXPECTED_EXE_HASH", hashlib.sha256(FIXTURE_EXE).hexdigest())
        self.size_patch = patch.object(importer, "EXPECTED_EXE_SIZE", len(FIXTURE_EXE))
        self.hash_patch.start()
        self.size_patch.start()
        self.addCleanup(self.hash_patch.stop)
        self.addCleanup(self.size_patch.stop)
        self.addCleanup(importer.safe_remove_tree, self.work, self.test_parent)

    def import_ok(self):
        result = importer.validate_and_import(self.source, self.output)
        self.assertTrue(result["success"], result)
        return result

    def contents(self, directory):
        return {name: path.read_bytes() for name, path in importer.collect_regular_files(directory).items()}

    def test_all_three_districts_are_required(self):
        (self.source / "data/ste.gmp").unlink()
        (self.source / "data/wil.scr").unlink()
        result = importer.validate_and_import(self.source, self.output)
        self.assertFalse(result["success"])
        self.assertEqual(result["missing_files"], ["data/ste.gmp", "data/wil.scr"])
        self.assertFalse(self.output.exists())

    def test_wrong_hash_does_not_create_output(self):
        (self.source / "gta2.exe").write_bytes(b"corrupt")
        result = importer.validate_and_import(self.source, self.output)
        self.assertFalse(result["success"])
        self.assertIn("expected_sha256", result)
        self.assertFalse(self.output.exists())

    def test_rejects_c_drive_relative_paths_and_drive_root(self):
        for path in (Path("C:/should-not-be-created"), Path("D:relative"), Path("D:/")):
            with self.subTest(path=path):
                self.assertFalse(importer.validate_and_import(self.source, path)["success"])

    def test_rejects_source_output_overlap(self):
        original = self.contents(self.source)
        for path in (self.source, self.source / "output", self.work):
            with self.subTest(path=path):
                self.assertFalse(importer.validate_and_import(self.source, path)["success"])
        self.assertEqual(original, self.contents(self.source))

    def test_rejects_private_data_inside_repository(self):
        with patch.object(importer, "REPOSITORY_ROOT", self.output):
            result = importer.validate_and_import(self.source, self.output)
        self.assertFalse(result["success"])
        self.assertFalse(self.output.exists())

    def test_import_packages_generated_data_and_leaves_source_unchanged(self):
        original = self.contents(self.source)
        result = self.import_ok()
        with zipfile.ZipFile(result["package_zip"]) as archive:
            self.assertEqual(set(archive.namelist()), set(original))
            for name, data in original.items():
                self.assertEqual(archive.read(name), data)
        self.assertEqual(original, self.contents(self.source))
        self.assertEqual(result["package_sha256"], importer.sha256_file(Path(result["package_zip"])))

    def test_case_insensitive_save_preservation_and_fresh_backups(self):
        source_save = self.source / "PLAYER" / "SAVE.SAV"
        source_save.parent.mkdir()
        source_save.write_bytes(b"old source save")
        first = self.import_ok()
        live_save = Path(first["staging_dir"]) / "player/save.sav"
        live_save.write_bytes(b"progress one")
        second = self.import_ok()
        self.assertEqual(live_save.read_bytes(), b"progress one")
        self.assertEqual((Path(second["save_backup_dir"]) / "save.sav").read_bytes(), b"progress one")
        live_save.write_bytes(b"progress two")
        third = self.import_ok()
        self.assertNotEqual(second["save_backup_dir"], third["save_backup_dir"])
        self.assertEqual((Path(third["save_backup_dir"]) / "save.sav").read_bytes(), b"progress two")
        self.assertEqual(live_save.read_bytes(), b"progress two")
        self.assertEqual(source_save.read_bytes(), b"old source save")
        with zipfile.ZipFile(third["package_zip"]) as archive:
            self.assertEqual(archive.read("player/save.sav"), b"progress two")

    def test_copy_failure_keeps_previous_install_and_archive(self):
        first = self.import_ok()
        stage = Path(first["staging_dir"])
        before, archive_before = self.contents(stage), Path(first["package_zip"]).read_bytes()
        with patch.object(importer, "_copy_file", side_effect=OSError("disk full")):
            result = importer.validate_and_import(self.source, self.output)
        self.assertFalse(result["success"])
        self.assertEqual(before, self.contents(stage))
        self.assertEqual(archive_before, Path(first["package_zip"]).read_bytes())
        self.assertEqual(list(self.output.glob(".gta2_staged.import-*")), [])

    def test_failed_archive_publish_rolls_back_directory_and_archive(self):
        first = self.import_ok()
        stage, package = Path(first["staging_dir"]), Path(first["package_zip"])
        before, archive_before = self.contents(stage), package.read_bytes()
        (self.source / "data/bil.gmp").write_bytes(b"updated fixture map")
        real_replace = os.replace

        def fail_archive(source, destination):
            if Path(source).name == "private.zip" and Path(destination) == package:
                raise OSError("simulated archive publication failure")
            return real_replace(source, destination)

        with patch.object(importer.os, "replace", side_effect=fail_archive):
            result = importer.validate_and_import(self.source, self.output)
        self.assertFalse(result["success"])
        self.assertEqual(before, self.contents(stage))
        self.assertEqual(archive_before, package.read_bytes())

    def make_junction(self, link, target):
        # Windows directory junctions need no symbolic-link privilege.
        quoted_link = str(link).replace("'", "''")
        quoted_target = str(target).replace("'", "''")
        subprocess.run([
            "powershell", "-NoProfile", "-NonInteractive", "-Command",
            f"New-Item -ItemType Junction -Path '{quoted_link}' -Target '{quoted_target}' -ErrorAction Stop | Out-Null",
        ], check=True, capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)

    def test_source_junction_is_rejected_without_reading_target(self):
        link = self.source / "linked-directory"
        outside = self.work / "outside"
        outside.mkdir()
        (outside / "secret").write_bytes(b"must not be imported")
        self.make_junction(link, outside)
        try:
            result = importer.validate_and_import(self.source, self.output)
            self.assertFalse(result["success"])
            self.assertIn("Links/Junctions", result["error"])
            self.assertFalse(self.output.exists())
        finally:
            link.rmdir()  # Removes the junction itself, never its target tree.

    def test_output_junction_is_rejected_without_writing_target(self):
        outside = self.work / "outside"
        outside.mkdir()
        self.make_junction(self.output, outside)
        try:
            result = importer.validate_and_import(self.source, self.output)
            self.assertFalse(result["success"])
            self.assertEqual(list(outside.iterdir()), [])
        finally:
            self.output.rmdir()

    def test_busy_output_is_not_modified(self):
        self.output.mkdir()
        lock = self.output / ".gta2_staged.import.lock"
        lock.write_text("other process", encoding="ascii")
        result = importer.validate_and_import(self.source, self.output)
        self.assertFalse(result["success"])
        self.assertEqual(lock.read_text(encoding="ascii"), "other process")
        self.assertFalse((self.output / "gta2_staged").exists())

    def test_private_container_update_is_atomic_and_preserves_saves(self):
        target = self.work / "container" / "gta2"
        first = prepare_container(self.source, target)
        self.assertTrue(first["success"])
        save = target / "PLAYER" / "save.sav"
        save.parent.mkdir(exist_ok=True)
        save.write_bytes(b"private progress")
        (self.source / "data/bil.sty").write_bytes(b"updated style fixture")
        result = prepare_container(self.source, target)
        self.assertTrue(result["success"])
        self.assertEqual(save.read_bytes(), b"private progress")
        self.assertEqual((target / "data/bil.sty").read_bytes(), b"updated style fixture")
        self.assertTrue((target / "gta2_wine_settings.reg").read_bytes().startswith(b"\xff\xfe"))
        self.assertFalse((self.source / "gta2_wine_settings.reg").exists())


if __name__ == "__main__":
    unittest.main()
