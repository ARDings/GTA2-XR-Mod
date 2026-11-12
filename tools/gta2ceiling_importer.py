#!/usr/bin/env python3
"""Stage private GTA2 data without changing its source. Never publish its output.

Writes, transaction files and backups stay outside this repository, under the
work root: the folder in GTA2CEILING_WORK_ROOT, or D:\GTA2Ceiling-work by default.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import uuid
import zipfile

EXPECTED_EXE_HASH = "420af867036b1a66ef94c54b8db2e7e654da359031987c6602bcfa74972e089f"
EXPECTED_EXE_SIZE = 1667072
MANDATORY_FILES = [
    "gta2.exe", "d3ddll.dll", "d3dpoly.dll", "mss32.dll", "polygon.dll",
    *[f"data/{district}.{extension}" for district in ("bil", "ste", "wil")
      for extension in ("gmp", "sty", "scr")], "data/e.gxt",
]
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


class ImportValidationError(ValueError):
    def __init__(self, message, **details):
        super().__init__(message)
        self.details = details


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _reject_reparse(path: Path) -> None:
    """Reject symlinks and Windows junctions, including dangling ones."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if stat.S_ISLNK(info.st_mode) or (
        getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    ):
        raise ValueError(f"Links/junctions are not allowed here: {path}")


def resolved_path(path: Path) -> Path:
    path = Path(path)
    if not path.is_absolute():
        raise ValueError(f"An absolute path is required: {path}")
    for component in reversed((path, *path.parents)):
        _reject_reparse(component)
    return path.resolve(strict=False)


def work_root() -> Path:
    """Where outputs go: GTA2CEILING_WORK_ROOT, or D:/GTA2Ceiling-work by default."""
    return Path(os.environ.get("GTA2CEILING_WORK_ROOT") or "D:/GTA2Ceiling-work")


def resolved_work_path(path: Path) -> Path:
    """An output path: under GTA2CEILING_WORK_ROOT when it is set, otherwise on D:."""
    path = resolved_path(path)
    if path == Path(path.anchor):
        raise ValueError(f"Safety rule: not a drive root: {path}")
    root = os.environ.get("GTA2CEILING_WORK_ROOT")
    if root:
        if not _contains(resolved_path(Path(root)), path):
            raise ValueError(f"Safety rule: output must be under GTA2CEILING_WORK_ROOT: {path}")
    elif path.drive.casefold() != "d:":
        raise ValueError(f"Safety rule: output must be on D:\\ (or set GTA2CEILING_WORK_ROOT): {path}")
    return path


def _contains(parent: Path, child: Path) -> bool:
    return child == parent or parent in child.parents


def _private_paths(source_dir: Path, target_dir: Path) -> tuple[Path, Path]:
    source = resolved_path(source_dir)
    target = resolved_work_path(target_dir)
    if not source.is_dir():
        raise ValueError(f"Source directory does not exist: {source}")
    if _contains(source, target) or _contains(target, source):
        raise ValueError("Source and target must not overlap.")
    if _contains(REPOSITORY_ROOT, target):
        raise ValueError("Private GTA2 data must not be placed inside the repository.")
    return source, target


def collect_regular_files(root: Path) -> dict[str, Path]:
    """Index relative names without case; reject links and ambiguous trees."""
    root = resolved_path(root)
    if not root.is_dir():
        raise ValueError(f"Kein Verzeichnis: {root}")
    files, names, pending = {}, set(), [root]
    while pending:
        directory = pending.pop()
        for child in sorted(directory.iterdir(), key=lambda p: p.name.casefold()):
            _reject_reparse(child)
            relative = child.relative_to(root).as_posix().casefold()
            if relative in names:
                raise ValueError(f"Ambiguous upper/lower case: {child}")
            names.add(relative)
            if child.is_dir():
                pending.append(child)
            elif stat.S_ISREG(child.stat().st_mode):
                files[relative] = child
            else:
                raise ValueError(f"Not a regular file: {child}")
    return dict(sorted(files.items()))


def _validate_install(source: Path) -> dict[str, Path]:
    files = collect_regular_files(source)
    missing = [name for name in MANDATORY_FILES if name not in files]
    if missing:
        raise ImportValidationError(
            "Incomplete GTA2 installation.", missing_files=missing,
            guidance="All three districts and the original libraries are required.",
        )
    exe = files["gta2.exe"]
    size, digest = exe.stat().st_size, sha256_file(exe)
    if size != EXPECTED_EXE_SIZE or digest != EXPECTED_EXE_HASH:
        raise ImportValidationError(
            "Unsupported GTA2 version.", actual_sha256=digest,
            expected_sha256=EXPECTED_EXE_HASH, actual_size=size,
            expected_size=EXPECTED_EXE_SIZE,
            guidance="Only the verified GTA2 9.6 executable is supported.",
        )
    return files


def _copy_file(source: Path, destination: Path) -> None:
    _reject_reparse(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        destination.chmod(stat.S_IWRITE | stat.S_IREAD)
    shutil.copy2(source, destination)
    destination.chmod(stat.S_IWRITE | stat.S_IREAD)


def safe_remove_tree(path: Path, parent: Path) -> None:
    """Delete only a checked, owned directory below its intended work-root parent."""
    parent, path = resolved_work_path(parent), resolved_work_path(path)
    if path == parent or not _contains(parent, path):
        raise ValueError(f"Unsafe delete target: {path}")
    if not path.exists():
        return
    collect_regular_files(path)

    def make_writable_and_retry(function, name, _error):
        Path(name).chmod(stat.S_IWRITE | stat.S_IREAD)
        function(name)

    shutil.rmtree(path, onerror=make_writable_and_retry)


@contextmanager
def output_lock(parent: Path, name: str):
    lock = resolved_work_path(parent / name)
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(descriptor, str(os.getpid()).encode("ascii"))
        os.close(descriptor)
        descriptor = None
        yield
    finally:
        if descriptor is not None:
            os.close(descriptor)
        lock.unlink()


def write_deterministic_zip(source: Path, destination: Path, prefix: str = "") -> None:
    """Stable ordering, timestamps and attributes for identical input bytes."""
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for _, path in collect_regular_files(source).items():
            relative = path.relative_to(source).as_posix()
            info = zipfile.ZipInfo(f"{prefix}/{relative}" if prefix else relative, ZIP_TIMESTAMP)
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with archive.open(info, "w") as target, path.open("rb") as content:
                shutil.copyfileobj(content, target, length=1024 * 1024)


def publish_transaction(pairs: list[tuple[Path, Path]], transaction: Path) -> None:
    """Publish prepared paths, rolling back earlier renames on an I/O error.

    Each rename is atomic on one volume, but a multi-file publish is not power-loss
    atomic. Retained transaction backups permit recovery after interruption.
    """
    transaction = resolved_work_path(transaction)
    changes = []
    try:
        for index, (prepared, destination) in enumerate(pairs):
            prepared, destination = resolved_work_path(prepared), resolved_work_path(destination)
            if (prepared == transaction or not _contains(transaction, prepared)
                    or destination.parent != transaction.parent):
                raise ValueError("Refused to publish outside the transaction bounds.")
            backup = transaction / f"previous-{index}"
            existed = destination.exists()
            if existed:
                os.replace(destination, backup)
            changes.append((destination, backup if existed else None, False))
            os.replace(prepared, destination)
            changes[-1] = (destination, backup if existed else None, True)
    except (OSError, ValueError):
        for index, (destination, backup, installed) in enumerate(reversed(changes)):
            if installed:
                os.replace(destination, transaction / f"aborted-{index}")
            if backup is not None:
                os.replace(backup, destination)
        raise


def stage_private_install(source_dir: Path, target_dir: Path, *, package_path: Path | None = None,
                          backup_existing: bool = True, extra_files: dict[str, bytes] | None = None) -> dict:
    source, target = _private_paths(source_dir, target_dir)
    source_files = _validate_install(source)
    parent = resolved_work_path(target.parent)
    package = resolved_work_path(package_path) if package_path else None
    if package and (package.parent != parent or package == target):
        raise ValueError("The private archive must be next to the target directory.")
    if package and package.exists() and not package.is_file():
        raise ValueError(f"Archive target is not a file: {package}")
    parent.mkdir(parents=True, exist_ok=True)
    with output_lock(parent, f".{target.name}.import.lock"):
        existing = collect_regular_files(target) if target.exists() else {}
        saves = {name: path for name, path in existing.items() if name.startswith("player/")}
        transaction = Path(tempfile.mkdtemp(prefix=f".{target.name}.import-", dir=parent))
        committed, rollback_complete, backup_dir = False, False, None
        try:
            candidate = transaction / "candidate"
            candidate.mkdir()
            # Names are normalized only in the private copy, never in the source.
            for relative, original in source_files.items():
                _copy_file(original, candidate / relative)
            for relative, original in saves.items():
                _copy_file(original, candidate / relative)
            (candidate / "player").mkdir(exist_ok=True)
            for relative, content in (extra_files or {}).items():
                relative_path = Path(relative)
                if relative_path.is_absolute() or relative_path.drive or ".." in relative_path.parts:
                    raise ValueError(f"Unsicherer Zusatzpfad: {relative}")
                extra = candidate / relative_path
                extra.parent.mkdir(parents=True, exist_ok=True)
                extra.write_bytes(content)
            _validate_install(candidate)
            pairs = [(candidate, target)]
            if package:
                prepared_zip = transaction / "private.zip"
                write_deterministic_zip(candidate, prepared_zip)
                pairs.append((prepared_zip, package))
            if backup_existing and saves:
                backup_root = resolved_work_path(parent / "player_backups")
                backup_root.mkdir(exist_ok=True)
                backup_dir = backup_root / (
                    datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ-") + uuid.uuid4().hex
                )
                saved = transaction / "save-backup"
                saved.mkdir()
                for relative, original in saves.items():
                    _copy_file(original, saved / Path(relative).relative_to("player"))
                os.replace(saved, backup_dir)
            try:
                publish_transaction(pairs, transaction)
            except (OSError, ValueError):
                rollback_complete = not any(transaction.glob("previous-*"))
                raise
            committed = True
            result = {
                "success": True, "copied_files_count": len(source_files),
                "preserved_save_files": len(saves), "staging_dir": str(target),
                "save_backup_dir": str(backup_dir) if backup_dir else None,
            }
            if package:
                result.update(package_zip=str(package), package_size_bytes=package.stat().st_size,
                              package_sha256=sha256_file(package))
            return result
        finally:
            # Keep recovery evidence if restoring an existing installation failed.
            if committed or rollback_complete or not any(transaction.glob("previous-*")):
                safe_remove_tree(transaction, parent)


def validate_and_import(source_dir: Path, output_dir: Path, backup_existing: bool = True) -> dict:
    try:
        output = resolved_work_path(output_dir)
        source, _ = _private_paths(source_dir, output)
        return stage_private_install(
            source, output / "gta2_staged",
            package_path=output / "gta2ceiling_private_import.zip",
            backup_existing=backup_existing,
        )
    except (OSError, ValueError) as error:
        return {"success": False, "error": str(error), **getattr(error, "details", {})}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Private GTA2 data import, not for redistribution")
    parser.add_argument("--source", type=Path, default=Path("D:/GTA2"))
    parser.add_argument("--output", type=Path, default=work_root() / "import")
    args = parser.parse_args()
    result = validate_and_import(args.source, args.output)
    print(json.dumps(result, indent=2, ensure_ascii=False))
    raise SystemExit(0 if result["success"] else 1)
