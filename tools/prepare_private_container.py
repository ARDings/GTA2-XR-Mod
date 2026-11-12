#!/usr/bin/env python3
"""Stage verified private files; this does not create a WinlatorXR container."""
import argparse
import json
from pathlib import Path

from gta2ceiling_importer import stage_private_install, work_root

REGISTRY_SETTINGS = """Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\\Software\\DMA Design Ltd\\GTA2\\Screen]
"full"=dword:00000001
"wide"=dword:00000000
"d3d"=dword:00000001
"depth"=dword:00000020
"res"=dword:00000002

[HKEY_CURRENT_USER\\Software\\DMA Design Ltd\\GTA2\\Audio]
"audio"=dword:00000001
"3d_sound"=dword:00000000
"""


def prepare_container(src_dir: Path, target_dir: Path) -> dict:
    return stage_private_install(
        src_dir, target_dir,
        extra_files={"gta2_wine_settings.reg": REGISTRY_SETTINGS.replace("\n", "\r\n").encode("utf-16")},
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Prepare a private GTA2 working copy for WinlatorXR")
    parser.add_argument("--source", type=Path, default=Path("D:/GTA2"))
    parser.add_argument("--target", type=Path, default=work_root() / "private-container" / "gta2")
    args = parser.parse_args()
    try:
        result = prepare_container(args.source, args.target)
    except (ValueError, OSError) as error:
        result = {"success": False, "error": str(error)}
    print(json.dumps(result, indent=2, ensure_ascii=False))
    raise SystemExit(0 if result["success"] else 1)
