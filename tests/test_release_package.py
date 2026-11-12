#!/usr/bin/env python3
"""Release regressions with synthetic PE headers, never original game files."""
import hashlib
import os
import shutil
import subprocess
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import build_release_package as release
import gta2ceiling_importer as importer

CANONICAL_CONFIG_DIR = Path(__file__).resolve().parent.parent / "configs"


def synthetic_pe(machine=0x14C, characteristics=0x2000):
    data = bytearray(256)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    struct.pack_into("<H", data, 86, characteristics)
    struct.pack_into("<H", data, 88, 0x10B)
    data[128:160] = b"synthetic test DLL, not game code"
    return bytes(data)


class TestReleasePackage(unittest.TestCase):
    def setUp(self):
        self.parent = importer.resolved_work_path(importer.work_root() / "test-artifacts")
        self.parent.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="release-", dir=self.parent))
        self.binaries, self.project, self.output = self.work / "bin", self.work / "project", self.work / "out"
        self.binaries.mkdir()
        self.project.mkdir()
        for name in release.REQUIRED_BINARIES:
            (self.binaries / name).write_bytes(synthetic_pe())
        for name in release.REQUIRED_DOCUMENTS:
            (self.project / name).write_bytes(f"Generated metadata: {name}\n".encode("utf-8"))
        (self.project / "configs").mkdir()
        (self.project / "configs/gta2ceiling.ini").write_bytes(b"[renderer]\nmode=takeover\n")
        (self.project / "configs/StartGTA2Ceiling.cmd").write_bytes(b"@echo off\n")
        for name in ("GTA2Ceiling.desktop", "SetupWinlatorShortcut.sh"):
            (self.project / "configs" / name).write_bytes((CANONICAL_CONFIG_DIR / name).read_bytes())
        self.root_patch = patch.object(release, "PROJECT_ROOT", self.project)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.addCleanup(importer.safe_remove_tree, self.work, self.parent)

    def build(self):
        return release.create_release_package(self.binaries, self.output, "test-1")

    def test_missing_dll_fails_closed_without_creating_release(self):
        (self.binaries / "gta2ceiling_vid.dll").unlink()
        with self.assertRaises(FileNotFoundError):
            self.build()
        self.assertFalse(self.output.exists())

    def test_wrong_architecture_and_non_dll_are_rejected(self):
        for content in (b"invalid", synthetic_pe(machine=0x8664), synthetic_pe(characteristics=0)):
            with self.subTest(content=content[:8]):
                (self.binaries / "gta2ceiling.dll").write_bytes(content)
                with self.assertRaises(ValueError):
                    self.build()
        self.assertFalse(self.output.exists())

    def test_missing_metadata_keeps_existing_release(self):
        first = self.build()
        archive = Path(first["package_zip"])
        before = archive.read_bytes()
        (self.project / "THIRD_PARTY_NOTICES.md").unlink()
        with self.assertRaises(FileNotFoundError):
            self.build()
        self.assertEqual(archive.read_bytes(), before)

    def test_allowlist_excludes_originals_and_tests(self):
        for name in ("gta2.exe", "bil.gmp", "test_synthetic_x86.exe", "xr_runtime_probe.exe", "mss32.dll"):
            (self.binaries / name).write_bytes(b"must never enter release")
        result = self.build()
        with zipfile.ZipFile(result["package_zip"]) as archive:
            names = {Path(name).name for name in archive.namelist()}
            expected = set(release.REQUIRED_BINARIES + release.REQUIRED_DOCUMENTS)
            expected.update(release.REQUIRED_CONFIGS)
            expected.update(("INSTALL.md", "install.reg", "SHA256SUMS.txt"))
            self.assertEqual(names, expected)
            self.assertEqual(result["staged_files_count"], len(expected))

    def test_archive_is_byte_identical_despite_input_mtime_changes(self):
        first = self.build()
        original = Path(first["package_zip"]).read_bytes()
        for file in self.binaries.iterdir():
            os.utime(file, (1234567890, 1234567890))
        second = self.build()
        self.assertEqual(original, Path(second["package_zip"]).read_bytes())
        self.assertEqual(first["package_sha256"], second["package_sha256"])

    def test_checksums_and_canonical_configuration_match_packaged_bytes(self):
        result = self.build()
        prefix = "GTA2Ceiling-vtest-1/"
        with zipfile.ZipFile(result["package_zip"]) as archive:
            checksums = archive.read(prefix + "SHA256SUMS.txt").decode("utf-8").splitlines()
            self.assertEqual(len(checksums), len(archive.namelist()) - 1)
            for entry in checksums:
                digest, name = entry.split("  ", 1)
                self.assertEqual(digest, hashlib.sha256(archive.read(prefix + name)).hexdigest())
            self.assertEqual(archive.read(prefix + "gta2ceiling.ini"),
                             (self.project / "configs/gta2ceiling.ini").read_bytes())
            for name in release.REQUIRED_CONFIGS:
                self.assertEqual(archive.read(prefix + name), (self.project / "configs" / name).read_bytes())
            self.assertTrue(all(info.date_time == importer.ZIP_TIMESTAMP for info in archive.infolist()))

    def test_missing_installer_keeps_existing_release(self):
        first = self.build()
        archive = Path(first["package_zip"])
        before = archive.read_bytes()
        (self.project / "configs/SetupWinlatorShortcut.sh").unlink()
        with self.assertRaises(FileNotFoundError):
            self.build()
        self.assertEqual(archive.read_bytes(), before)

    def test_invalid_shortcut_or_script_is_rejected_before_publication(self):
        desktop = self.project / "configs/GTA2Ceiling.desktop"
        original = desktop.read_bytes()
        for modified in (
            original.replace(b"cmd.exe", b"other.exe"),
            original.replace(b"xr_colour_key=3", b"xr_colour_key=0"),
            original.replace(b"gcc_exact_colour_key=1", b"gcc_exact_colour_key=0"),
            original.replace(b"autoclose=0", b"autoclose=1"),
            original + b"execArgs=/c something-else\n",
            original.replace(b"\n", b"\r\n"),
        ):
            with self.subTest(modified=modified):
                desktop.write_bytes(modified)
                with self.assertRaises(ValueError):
                    self.build()
                self.assertFalse(self.output.exists())
        desktop.write_bytes(original)
        script = self.project / "configs/SetupWinlatorShortcut.sh"
        script.write_bytes(script.read_bytes().replace(b"\n", b"\r\n"))
        with self.assertRaises(ValueError):
            self.build()
        self.assertFalse(self.output.exists())

    def test_rejects_unsafe_output_and_version(self):
        with self.assertRaises(ValueError):
            release.create_release_package(self.binaries, Path("C:/release-forbidden"))
        for version in ("../escape", "1/../../escape", "", "\\absolute"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                release.create_release_package(self.binaries, self.output, version)
        self.assertFalse(self.output.exists())

    def test_publish_failure_restores_previous_release(self):
        first = self.build()
        archive = Path(first["package_zip"])
        before = archive.read_bytes()
        (self.project / "README.md").write_bytes(b"new metadata")
        real_replace = os.replace

        def fail_zip(source, destination):
            if Path(source).name == "release.zip" and Path(destination) == archive:
                raise OSError("simulated publishing failure")
            return real_replace(source, destination)

        with patch.object(importer.os, "replace", side_effect=fail_zip), self.assertRaises(OSError):
            self.build()
        self.assertEqual(archive.read_bytes(), before)
        self.assertEqual((self.output / "GTA2Ceiling-vtest-1/README.md").read_bytes(), b"Generated metadata: README.md\n")


class TestWinlatorShortcutSetup(unittest.TestCase):
    """Execute the real POSIX script against generated container trees under the work root.

    Git for Windows uses MSYS synthetic symlinks here, so no administrator or
    Windows Developer Mode is needed. This exercises shell path/INI logic, not
    an Android device or a working GTA2 installation.
    """
    def setUp(self):
        candidates = [os.environ.get("GTA2CEILING_TEST_SH"), shutil.which("sh"),
                      r"C:\Program Files\Git\bin\sh.exe"]
        self.shell = next((candidate for candidate in candidates if candidate and Path(candidate).is_file()), None)
        if not self.shell:
            self.fail("Shell setup tests require sh (Git for Windows), or GTA2CEILING_TEST_SH.")
        self.parent = importer.resolved_work_path(importer.work_root() / "test-artifacts")
        self.parent.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="shortcut-", dir=self.parent))
        self.addCleanup(importer.safe_remove_tree, self.work, self.parent)
        self.imagefs = self.work / "imagefs"
        self.home = self.imagefs / "home"
        self.active = self.home / "xuser-4"
        self.desktop = self.active / ".wine/drive_c/users/xuser/Desktop"
        self.other_desktop = self.home / "xuser-3/.wine/drive_c/users/xuser/Desktop"
        self.desktop.mkdir(parents=True)
        self.other_desktop.mkdir(parents=True)
        self.other_file = self.other_desktop / "GTA2Ceiling.desktop"
        self.other_file.write_bytes(b"Do not touch another container.\n")
        self.target = self.desktop / "GTA2Ceiling.desktop"
        self.game = self.work / "shared/Download/GTA2Ceiling/gta2"
        self.game.mkdir(parents=True)
        (self.game / "data").mkdir()
        for name in ("SetupWinlatorShortcut.sh", "GTA2Ceiling.desktop"):
            (self.game / name).write_bytes((CANONICAL_CONFIG_DIR / name).read_bytes())
        for name in ("gta2.exe", "gta2ceiling.dll", "gta2ceiling_vid.dll", "gta2ceiling.ini", "install.reg", "StartGTA2Ceiling.cmd"):
            (self.game / name).write_bytes(b"Synthetic presence fixture; never executed.\n")
        self.script = self.game / "SetupWinlatorShortcut.sh"
        self.template = (self.game / "GTA2Ceiling.desktop").read_bytes()
        self.env = os.environ.copy()
        self.env["MSYS"] = "winsymlinks:sys"
        # MSYS maps TEMP to /tmp. Keep it below, not above, the fixture paths
        # so pwd/readlink use the same spelling during these POSIX path checks.
        shell_temp = self.work / "shell-temp"
        shell_temp.mkdir()
        self.env["TEMP"] = str(shell_temp)
        self.env["TMP"] = str(shell_temp)
        self.link(self.active, self.home / "xuser")

    @staticmethod
    def shell_path(path):
        text = Path(path).absolute().as_posix()
        return "/" + text[0].lower() + text[2:] if len(text) > 1 and text[1] == ":" else text

    def link(self, source, target):
        result = subprocess.run([self.shell, "-c", 'ln -s "$1" "$2"', "fixture-link",
                                 self.shell_path(source), self.shell_path(target)],
                                cwd=self.work, env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # On POSIX, unlink symlinks before the shared safe tree cleanup. On MSYS
        # these are ordinary special files to Python, and unlink works too.
        self.addCleanup(lambda: target.unlink() if target.is_symlink() or target.exists() else None)

    def run_setup(self, expected=0, *, cwd=None, script=None, args=()):
        result = subprocess.run([self.shell, self.shell_path(script or self.script), *args],
                                cwd=cwd or self.imagefs, env=self.env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertEqual(self.other_file.read_bytes(), b"Do not touch another container.\n")
        self.assertFalse((self.desktop / ".GTA2Ceiling.shortcut.lock").exists())
        self.assertFalse(list(self.desktop.glob(".GTA2Ceiling.desktop.*")))
        return result

    def test_installs_only_selected_container_and_is_idempotent(self):
        self.run_setup()
        self.assertEqual(self.target.read_bytes(), self.template)
        previous_stat = self.target.stat()
        second = self.run_setup()
        self.assertIn("Already configured", second.stdout)
        self.assertEqual(self.target.stat().st_mtime_ns, previous_stat.st_mtime_ns)
        self.assertEqual({path.name for path in self.desktop.iterdir()}, {"GTA2Ceiling.desktop"})

    def test_matching_old_launcher_is_backed_up_once(self):
        old = self.template.replace(b"use_pt=1\nxr_colour_key=3\nxr_colour_key_tolerance=10\n", b"")
        self.target.write_bytes(old.replace(b"\n", b"\r\n"))
        self.run_setup()
        backups = list(self.desktop.glob("GTA2Ceiling.desktop.backup.*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), old.replace(b"\n", b"\r\n"))
        self.assertEqual(self.target.read_bytes(), self.template)
        self.run_setup()
        self.assertEqual(list(self.desktop.glob("GTA2Ceiling.desktop.backup.*")), backups)

    def test_foreign_exec_args_and_duplicate_fields_are_not_replaced(self):
        for old in (self.template.replace(b"cmd.exe", b"other.exe"),
                    self.template.replace(b"StartGTA2Ceiling.cmd", b"OtherApp.cmd"),
                    self.template.replace(b"Exec=wine", b"Exec=wine other.exe\nExec=wine"),
                    self.template.replace(b"Exec=wine", b"  Exec =other.exe\nExec=wine")):
            with self.subTest(old=old):
                self.target.write_bytes(old)
                self.run_setup(1)
                self.assertEqual(self.target.read_bytes(), old)
                self.assertFalse(list(self.desktop.glob("GTA2Ceiling.desktop.backup.*")))

    def test_selected_home_cannot_escape_imagefs_home(self):
        (self.home / "xuser").unlink()
        foreign = self.work / "outside/xuser-99"
        foreign.mkdir(parents=True)
        self.link(foreign, self.home / "xuser")
        self.run_setup(1)
        self.assertFalse(self.target.exists())
        self.assertEqual(list(foreign.iterdir()), [])

    def test_selected_home_must_be_the_active_link(self):
        (self.home / "xuser").unlink()
        (self.home / "xuser").mkdir()
        self.run_setup(1)
        self.assertFalse(self.target.exists())
        (self.home / "xuser").rmdir()

    def test_redirected_desktop_is_rejected(self):
        self.desktop.rmdir()
        self.link(self.other_desktop, self.desktop)
        self.run_setup(1)
        self.assertEqual(self.other_file.read_bytes(), b"Do not touch another container.\n")

    def test_target_symlink_is_rejected(self):
        self.link(self.other_file, self.target)
        self.run_setup(1)
        self.assertEqual(self.other_file.read_bytes(), b"Do not touch another container.\n")

    def test_template_is_data_and_invalid_content_does_not_execute(self):
        marker = self.work / "must-not-exist"
        attack = ("Exec=$(touch " + self.shell_path(marker) + ")").encode()
        (self.game / "GTA2Ceiling.desktop").write_bytes(self.template.replace(b"Exec=wine C:/windows/system32/cmd.exe", attack))
        self.run_setup(1)
        self.assertFalse(marker.exists())
        self.assertFalse(self.target.exists())

    def test_missing_game_file_rejects_installation(self):
        (self.game / "data").rmdir()
        first = self.run_setup(1)
        self.assertIn("game data directory is missing", first.stderr)
        (self.game / "data").mkdir()
        (self.game / "gta2.exe").unlink()
        second = self.run_setup(1)
        self.assertIn("gta2.exe", second.stderr)
        self.assertFalse(self.target.exists())

    def test_wrong_cwd_source_path_and_arguments_are_rejected(self):
        self.run_setup(1, cwd=self.work)
        wrong = self.work / "wrong-folder"
        wrong.mkdir()
        for file in self.game.iterdir():
            if file.is_dir():
                (wrong / file.name).mkdir()
            else:
                (wrong / file.name).write_bytes(file.read_bytes())
        self.run_setup(1, script=wrong / "SetupWinlatorShortcut.sh")
        self.run_setup(1, args=("--container", "3"))
        self.assertFalse(self.target.exists())


if __name__ == "__main__":
    unittest.main()
