#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare only pinned, small native dependencies under the work root. Never fetch Wine/game assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import urllib.request
import zipfile


def work_root():
    """Where build outputs go: GTA2CEILING_WORK_ROOT, or D:\\GTA2Ceiling-work."""
    return Path(os.environ.get("GTA2CEILING_WORK_ROOT") or r"D:\GTA2Ceiling-work")


def under_work_root(path):
    """Builds stay outside the source tree: under GTA2CEILING_WORK_ROOT when set, else on D:."""
    root = os.environ.get("GTA2CEILING_WORK_ROOT")
    if root:
        root = Path(root).resolve()
        return path == root or root in path.parents
    return path.drive.lower() == "d:"


def checked_path(path):
    path = path.absolute()
    if not under_work_root(path):
        raise ValueError("Dependency outputs must be under the work root (GTA2CEILING_WORK_ROOT or D:)")
    for ancestor in (path, *path.parents):
        if ancestor.is_symlink() or ancestor.is_junction():
            raise ValueError(f"Reparse dependency output rejected: {ancestor}")
    return path


def matches(data, expected):
    return hashlib.sha256(data).hexdigest().lower() == expected.lower()


def save_verified(path, data, expected):
    if not matches(data, expected):
        raise ValueError(f"Source hash mismatch: {path.name}")
    path = checked_path(path)
    if path.exists():
        if not matches(path.read_bytes(), expected):
            raise ValueError(f"Existing dependency differs; not overwritten: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader-aar", type=Path, help="Reviewed Unity OpenXR 1.1.49 loader AAR; read-only input")
    parser.add_argument("--openxr-output", type=Path, default=work_root() / "roomprobe" / "deps")
    parser.add_argument("--sdl-output", type=Path, default=work_root() / "implementation" / "winlator-host" / "native-build" / "deps" / "SDL2")
    parser.add_argument("--offline", action="store_true", help="Require all SDL headers to be present with pinned hashes")
    parser.add_argument("--verify-only", action="store_true", help="Check all existing dependency files; no archive extraction or network")
    args = parser.parse_args()
    lock = json.loads((Path(__file__).parent / "dependency-lock.json").read_text(encoding="utf-8"))
    checked_path(args.openxr_output)
    checked_path(args.sdl_output)
    if args.verify_only:
        for entry in lock["openxr"]["files"]:
            existing = args.openxr_output / entry["name"]
            save_verified(existing, existing.read_bytes(), entry["sha256"])
    else:
        if not args.loader_aar:
            parser.error("--loader-aar is required unless --verify-only is used")
        if not matches(args.loader_aar.read_bytes(), lock["openxr"]["aarSha256"]):
            raise ValueError("Loader AAR does not match dependency-lock.json")
        with zipfile.ZipFile(args.loader_aar) as archive:
            for entry in lock["openxr"]["files"]:
                save_verified(args.openxr_output / entry["name"], archive.read(entry["name"]), entry["sha256"])
    for entry in lock["sdl"]["files"]:
        name = entry["name"]
        if Path(name).name != name:
            raise ValueError("Unexpected SDL lock path")
        output = args.sdl_output / name
        if output.exists():
            save_verified(output, output.read_bytes(), entry["sha256"])
            continue
        if args.offline or args.verify_only:
            raise ValueError(f"Missing pinned SDL header: {output}")
        source_path = name if name == "LICENSE.txt" else "include/" + name
        url = f"https://raw.githubusercontent.com/libsdl-org/SDL/{lock['sdl']['commit']}/{source_path}"
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read(4 * 1024 * 1024 + 1)
        save_verified(output, data, entry["sha256"])
    print(f"Verified {len(lock['openxr']['files'])} OpenXR files and {len(lock['sdl']['files'])} SDL files; no runtime/game assets.")


if __name__ == "__main__":
    main()
