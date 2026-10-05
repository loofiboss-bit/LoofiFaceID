#!/usr/bin/env python3
"""Build a deterministic archive from the explicit, distributable source list."""
from __future__ import annotations

import argparse
import gzip
import json
import os
import subprocess
import tarfile
import tempfile
from io import BytesIO
from pathlib import Path, PurePosixPath

from verify_project_identity import identity, verify


def source_files(root: Path) -> list[str]:
    entries = (root / "packaging/fedora/source-files.txt").read_text(encoding="utf-8").splitlines()
    if not entries or len(entries) > 10000 or entries != sorted(set(entries)):
        raise ValueError("source manifest must be non-empty, sorted and unique")
    for entry in entries:
        path = PurePosixPath(entry)
        if path.is_absolute() or any(part in ("..", ".git") for part in path.parts) or str(path) != entry:
            raise ValueError("unsafe source manifest entry")
        source = root / entry
        if not source.is_file() or source.is_symlink() or not source.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"missing or unsafe source file: {entry}")
    return entries


def provenance(root: Path, values: dict[str, str]) -> dict:
    stored = root / "SOURCE-PROVENANCE.json"
    if stored.is_file():
        record = json.loads(stored.read_text(encoding="utf-8"))
        if record.get("version") != values["version"] or record.get("name") != values["name"]:
            raise ValueError("source provenance identity mismatch")
        return record
    commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True, check=False)
    epoch = subprocess.run(["git", "-C", str(root), "log", "-1", "--format=%ct"], capture_output=True, text=True, check=False)
    return {"name": values["name"], "version": values["version"],
            "commit": commit.stdout.strip() if commit.returncode == 0 else None,
            "source_date_epoch": int(os.environ.get("SOURCE_DATE_EPOCH", epoch.stdout.strip() or "0"))}


def create_archive(root: Path, output: Path) -> None:
    values = identity(root)
    verify(root, values)
    entries = source_files(root)
    record = provenance(root, values)
    prefix = f"{values['name']}-{values['version']}"
    directories = {prefix}
    for entry in entries:
        directories.update(f"{prefix}/{parent}" for parent in PurePosixPath(entry).parents if str(parent) != ".")
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix="kfaceauth-source-", suffix=".tar.gz", dir=output.parent)
    try:
        with os.fdopen(descriptor, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for directory in sorted(directories):
                    info = tarfile.TarInfo(directory)
                    info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, record["source_date_epoch"]
                    archive.addfile(info)
                for entry in entries:
                    source = root / entry
                    info = tarfile.TarInfo(f"{prefix}/{entry}")
                    info.size, info.mtime = source.stat().st_size, record["source_date_epoch"]
                    info.mode = 0o755 if source.stat().st_mode & 0o111 else 0o644
                    with source.open("rb") as stream:
                        archive.addfile(info, stream)
                data = (json.dumps(record, sort_keys=True) + "\n").encode()
                info = tarfile.TarInfo(f"{prefix}/SOURCE-PROVENANCE.json")
                info.size, info.mode, info.mtime = len(data), 0o644, record["source_date_epoch"]
                archive.addfile(info, BytesIO(data))
        os.chmod(temporary, 0o644)
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        output = args.output or args.root / identity(args.root)["archive"]
        create_archive(args.root, output)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Source archive rejected: {error}\n")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
