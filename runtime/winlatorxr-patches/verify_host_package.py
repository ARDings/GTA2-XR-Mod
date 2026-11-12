#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the actual thin APK for private-package path and stale shim regressions.

This is artifact evidence, not a controller or headset acceptance test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def verify(apk, fork):
    forbidden = (b"/data/data/com.winlator.cmod/", b"/data/user/0/com.winlator.cmod/")
    own_gamepad = b"/data/data/com.gta2ceiling.winlator/files/imagefs/tmp/gamepad%s.mem"
    with zipfile.ZipFile(apk) as package:
        shim = package.read("lib/arm64-v8a/libevshim.so")
        if own_gamepad not in shim:
            raise ValueError("Packaged controller shim does not target this app's private memory")
        for name in package.namelist():
            if not (name.endswith(".so") or name.endswith(".dex")):
                continue
            data = package.read(name)
            if any(path in data for path in forbidden):
                raise ValueError(f"Original-app private path in executable APK member: {name}")
        assets = [name for name in package.namelist() if name.startswith("assets/")]
        if any(name != "assets/PublicSuffixDatabase.list" for name in assets):
            raise ValueError("Unexpected bundled asset; thin host must not bundle donor/game files")
        xr = package.read("lib/arm64-v8a/libxr.so")
    source = (fork / "app/src/main/cpp/winlator/evshim.c").read_text(encoding="utf-8")
    launcher = (fork / "app/src/main/java/com/winlator/cmod/xenvironment/components/BionicProgramLauncherComponent.java").read_text(encoding="utf-8")
    if 'pread(fd, &cur, sizeof cur, 0)' not in source:
        raise ValueError("Shared gamepad snapshots must not advance to EOF")
    if 'imageFs.getLibDir() + "/libevshim.so"' in launcher:
        raise ValueError("Launcher still selects the unpatched donor ImageFS controller shim")
    if 'context.getApplicationInfo().nativeLibraryDir + "/libevshim.so"' not in launcher:
        raise ValueError("Launcher does not select this APK's built controller shim")
    return {"apk": str(apk), "size": apk.stat().st_size,
            "sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
            "libxrSha256": hashlib.sha256(xr).hexdigest(),
            "libevshimSha256": hashlib.sha256(shim).hexdigest(),
            "privatePackagePathCheck": "PASS", "assets": assets,
            "deviceTested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("fork", type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.apk, args.fork), indent=2))
