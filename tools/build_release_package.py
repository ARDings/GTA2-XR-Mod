#!/usr/bin/env python3
"""Package the actual x86 mod DLLs from an explicit allowlist, never GTA2 data."""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile

from gta2ceiling_importer import (
    collect_regular_files, output_lock, publish_transaction, resolved_work_path, work_root,
    resolved_path, safe_remove_tree, sha256_file, write_deterministic_zip,
)

PROJECT_ROOT = Path(__file__).resolve().parent.parent
REQUIRED_BINARIES = ("gta2ceiling.dll", "gta2ceiling_vid.dll")
REQUIRED_DOCUMENTS = ("LICENSE", "README.md", "DEPENDENCIES.md", "THIRD_PARTY_NOTICES.md")
REQUIRED_CONFIGS = ("gta2ceiling.ini", "StartGTA2Ceiling.cmd", "GTA2Ceiling.desktop", "SetupWinlatorShortcut.sh")
SHORTCUT_SECTIONS = {
    "Desktop Entry": {
        "Name": "GTA2Ceiling", "Type": "Application", "Exec": "wine C:/windows/system32/cmd.exe",
    },
    "Extra Data": {
        "autoclose": "0", "execArgs": r"/d /c D:\GTA2Ceiling\gta2\StartGTA2Ceiling.cmd",
        "screenSize": "3072x1536", "useReshade": "3", "use_pt": "1",
        "xr_colour_key": "3", "xr_colour_key_tolerance": "10", "gcc_exact_colour_key": "1",
    },
}
INSTALL_GUIDE = """# GTA2Ceiling: experimental Quest installation

This is a mod package, not GTA2 or a Quest APK. Obtain your own supported GTA2 9.6
installation separately. Full-ceiling room scan requires the separate
GTA2Ceiling Host APK and its pinned original WinlatorXR Dawn42 asset donor
(versionCode 24); keep both apps installed. Host source/build instructions are in
runtime/winlatorxr-patches/README.md in the source repository. This mod ZIP does
not install those apps. Local tests do not replace device acceptance.

1. Back up your original GTA2 installation, player saves, mod configuration and
   existing GTA2 registry settings. Keep the original installation unchanged.
2. On Quest, put the complete private game copy in:
       /sdcard/Download/GTA2Ceiling/gta2/
   This directory must contain gta2.exe and data/. Copy the two mod DLLs,
   gta2ceiling.ini, install.reg, StartGTA2Ceiling.cmd, GTA2Ceiling.desktop and
   SetupWinlatorShortcut.sh into that same directory. Do not replace player saves.
   This directory corresponds to D:\\GTA2Ceiling\\gta2 in Wine when D: is mapped
   to the Quest Download directory. Configure that drive mapping in your container.
3. Select your own intended container in GTA2Ceiling Host and start its Wine desktop
   once to finish initialization. Close the running session before setup. Open
   that selected container's Debug Terminal. Its current directory must be the
   app's imagefs directory, with home/xuser pointing at the selected container.
   Run this single command:
       sh /sdcard/Download/GTA2Ceiling/gta2/SetupWinlatorShortcut.sh
4. The setup validates its source files and active-container path, then writes
   only this container's Desktop/GTA2Ceiling.desktop. An identical shortcut is
   unchanged. A previous matching GTA2Ceiling cmd launcher is backed up before
   update; another Exec/argument combination, a link or an ambiguous shortcut is
   rejected without replacement. It never edits other containers or game files.
   Read the terminal result. Refresh the shortcut list and launch GTA2Ceiling.
5. Pair the Xbox controller with Quest and expose it as XInput in that container.
   The launcher sets the XrAPI 0.6 handshake and imports registry settings INSIDE
   Wine. Never import install.reg into your Windows host merely for a Quest setup.
   Restart the container once if its host had previously initialized an older
   XrAPI protocol. The shortcut uses autoclose=0 to retain the session for diagnosis.
6. Allow room access when the Host requests it. The default room_scan=1 reads the
   existing Quest room scan and uses its complete ceiling boundary. Until a valid
   ceiling is available, a status message is shown. Create a room scan in Meta's
   room setup if none exists. No replacement rectangle is silently invented.
   The room remains visible outside the ceiling. The shortcut enables the Host's
   exact magenta key (gcc_exact_colour_key=1). MSAA and filtered mipmaps default to
   2 samples and 4x anisotropy in [graphics]; 0 samples disables MSAA independently.
   Both thumbsticks together open placement; sticks move/rotate the city, LB/RB
   change its scale, A saves, B cancels. The physical ceiling shape stays fixed.
   For a legacy stock-host diagnostic only, room_scan=0 enables the manual board
   and its broad pink hue key. That mode does not fulfill full-ceiling acceptance.
7. To roll back, close the session, remove only this GTA2Ceiling shortcut and
   restore its recorded .backup file if one was made. Restore your previous
   registry settings and mod configuration/files from backup. Keep data/ and player/.

The setup does not create a container, download a runtime/game, or change drive
mappings. If it fails, fix the stated path/container issue; do not run it with a
guessed container number. Do not edit its Exec string to point at another folder.

SHA256SUMS.txt covers every payload file except itself. The optional private-import
ZIP contains original game data and must never be uploaded with this public mod.
The .desktop file is interpreted as data; the setup never sources or evaluates it.
"""


def _registry_settings() -> bytes:
    lines = ["Windows Registry Editor Version 5.00", ""]
    for hive, prefix in (
        ("HKEY_CURRENT_USER", "Software"),
        ("HKEY_LOCAL_MACHINE", "Software"),
        ("HKEY_LOCAL_MACHINE", "Software\\WOW6432Node"),
    ):
        lines.extend([
            f"[{hive}\\{prefix}\\DMA Design Ltd\\GTA2\\Screen]",
            '"rendername"="gta2ceiling.dll"', '"videoname"="gta2ceiling_vid.dll"',
            '"lighting"=dword:00000001', '"max_frame_rate"=dword:00000000',
            '"min_frame_rate"=dword:00000000', '"do_play_movie"=dword:00000000', "",
        ])
    return ("\r\n".join(lines) + "\r\n").encode("utf-16")


def validate_x86_dll(path: Path) -> None:
    """Reject absent/truncated/wrong-architecture binaries before publishing."""
    path = resolved_path(path)
    with path.open("rb") as stream:
        dos = stream.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError(f"Not a PE DLL: {path}")
        pe_offset = struct.unpack_from("<I", dos, 0x3C)[0]
        if pe_offset < 64 or pe_offset > path.stat().st_size - 26:
            raise ValueError(f"Invalid PE header: {path}")
        stream.seek(pe_offset)
        header = stream.read(26)
        if (header[:4] != b"PE\0\0" or struct.unpack_from("<H", header, 4)[0] != 0x14C
                or not struct.unpack_from("<H", header, 22)[0] & 0x2000
                or struct.unpack_from("<H", header, 24)[0] != 0x10B):
            raise ValueError(f"A PE32/x86 DLL is required: {path}")


def validate_shortcut_assets(config_directory: Path) -> None:
    """Require the canonical launch target and MR settings before publishing."""
    desktop = (config_directory / "GTA2Ceiling.desktop").read_bytes()
    script = (config_directory / "SetupWinlatorShortcut.sh").read_bytes()
    if b"\0" in desktop or b"\r" in desktop or not desktop.endswith(b"\n"):
        raise ValueError("GTA2Ceiling.desktop must be NUL-free UTF-8 with LF line endings.")
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        parser.read_string(desktop.decode("utf-8"))
    except (UnicodeError, configparser.Error) as error:
        raise ValueError(f"Invalid GTA2Ceiling shortcut: {error}") from error
    sections = {name: dict(parser[name]) for name in parser.sections()}
    if parser.defaults() or sections != SHORTCUT_SECTIONS:
        raise ValueError("GTA2Ceiling shortcut target/settings differ from the canonical container launcher.")
    # The shell installer validates this exact ordered template without eval.
    expected = "\n\n".join(
        "\n".join([f"[{name}]", *[f"{key}={value}" for key, value in fields.items()]])
        for name, fields in SHORTCUT_SECTIONS.items()
    ).encode("utf-8") + b"\n"
    if desktop != expected:
        raise ValueError("GTA2Ceiling.desktop must retain the installer's canonical line order.")
    if not script.startswith(b"#!/system/bin/sh\n") or b"\r" in script or b"\0" in script:
        raise ValueError("SetupWinlatorShortcut.sh must be an Android sh script with LF line endings and no NUL.")


def create_release_package(build_bin_dir: Path, output_dir: Path, version: str = "0.1.0") -> dict:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", version):
        raise ValueError("Invalid release version.")
    binaries = resolved_work_path(build_bin_dir)
    output = resolved_work_path(output_dir)
    root = resolved_path(PROJECT_ROOT)
    payload = {name: binaries / name for name in REQUIRED_BINARIES}
    payload.update({name: root / name for name in REQUIRED_DOCUMENTS})
    payload.update({name: root / "configs" / name for name in REQUIRED_CONFIGS})
    for name, source in payload.items():
        checked = resolved_path(source)
        if not checked.is_file():
            raise FileNotFoundError(f"Pflichtdatei fehlt: {name} ({source})")
    validate_shortcut_assets(root / "configs")
    for name in REQUIRED_BINARIES:
        validate_x86_dll(payload[name])

    package_name = f"GTA2Ceiling-v{version}"
    stage, archive = output / package_name, output / f"{package_name}.zip"
    resolved_work_path(stage)
    resolved_work_path(archive)
    if stage.exists() and not stage.is_dir():
        raise ValueError(f"Release path is not a directory: {stage}")
    if archive.exists() and not archive.is_file():
        raise ValueError(f"Archive target is not a file: {archive}")
    # A release must never rename away its own input directory or source project.
    for source in (*payload.values(), root):
        if source == stage or stage in source.parents:
            raise ValueError("Release target and inputs must not overlap.")
    if stage.exists():
        collect_regular_files(stage)
    output.mkdir(parents=True, exist_ok=True)
    with output_lock(output, f".{package_name}.release.lock"):
        transaction = Path(tempfile.mkdtemp(prefix=f".{package_name}.release-", dir=output))
        committed = False
        try:
            candidate = transaction / "candidate"
            candidate.mkdir()
            for name, source in payload.items():
                shutil.copyfile(source, candidate / name)
            for name in REQUIRED_BINARIES:
                validate_x86_dll(candidate / name)
            validate_shortcut_assets(candidate)
            (candidate / "INSTALL.md").write_bytes(INSTALL_GUIDE.encode("utf-8"))
            (candidate / "install.reg").write_bytes(_registry_settings())
            checksums = {
                path.name: sha256_file(path)
                for path in sorted(candidate.iterdir(), key=lambda item: item.name)
            }
            (candidate / "SHA256SUMS.txt").write_bytes("".join(
                f"{digest}  {name}\n" for name, digest in checksums.items()
            ).encode("utf-8"))
            prepared_zip = transaction / "release.zip"
            write_deterministic_zip(candidate, prepared_zip, prefix=package_name)
            publish_transaction([(candidate, stage), (prepared_zip, archive)], transaction)
            committed = True
            return {
                "success": True, "package_zip": str(archive),
                "package_size_bytes": archive.stat().st_size,
                "package_sha256": sha256_file(archive),
                "staged_files_count": len(checksums) + 1, "checksums": checksums,
            }
        finally:
            if committed or not any(transaction.glob("previous-*")):
                safe_remove_tree(transaction, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Public GTA2Ceiling mod package without game data")
    parser.add_argument("--build-bin", type=Path, default=work_root() / "build-x86" / "bin" / "Release")
    parser.add_argument("--output", type=Path, default=work_root() / "release")
    parser.add_argument("--version", default="0.1.0")
    args = parser.parse_args()
    try:
        result = create_release_package(args.build_bin, args.output, args.version)
    except (ValueError, OSError) as error:
        result = {"success": False, "error": str(error)}
    print(json.dumps(result, indent=2, ensure_ascii=False))
    raise SystemExit(0 if result["success"] else 1)
