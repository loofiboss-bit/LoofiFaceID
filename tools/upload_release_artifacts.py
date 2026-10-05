#!/usr/bin/env python3
"""Validate release lineage and upload without replacing published bytes."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path

from verify_project_identity import identity, verify


def run(*command: str) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise ValueError(f"release command failed: {command[0]}")
    return result.stdout


def validate_lineage(root: Path, artifacts: Path, tag: str) -> None:
    values = identity(root)
    verify(root, values)
    if tag != f"v{values['version']}":
        raise ValueError("release tag differs from canonical version")
    head = run("git", "-C", str(root), "rev-parse", "HEAD").strip()
    target = run("git", "-C", str(root), "rev-parse", f"refs/tags/{tag}^{{commit}}").strip()
    if head != target or not re.fullmatch(r"[0-9a-f]{40,64}", head):
        raise ValueError("checkout differs from release tag commit")
    run("git", "-C", str(root), "diff", "--exit-code", "HEAD", "--")
    run(str(root / "packaging/fedora/verify-release-artifacts.sh"), str(artifacts))
    with tarfile.open(artifacts / values["archive"], "r:gz") as archive:
        name = f"{values['name']}-{values['version']}/SOURCE-PROVENANCE.json"
        matches = [entry for entry in archive.getmembers() if entry.name == name]
        if len(matches) != 1 or not matches[0].isfile() or matches[0].size > 4096:
            raise ValueError("source archive has invalid lineage record")
        stream = archive.extractfile(matches[0])
        if stream is None:
            raise ValueError("source lineage record missing")
        record = json.load(stream)
    if record.get("commit") != head or record.get("version") != values["version"] or record.get("name") != values["name"]:
        raise ValueError("archive lineage differs from release checkout")


def upload(root: Path, artifacts: Path, tag: str, repository: str) -> None:
    validate_lineage(root, artifacts, tag)
    expected = {entry.name: entry for entry in artifacts.iterdir() if entry.is_file()}
    data = json.loads(run("gh", "release", "view", tag, "--repo", repository, "--json", "assets"))
    published = [asset["name"] for asset in data["assets"]]
    if len(published) != len(set(published)) or set(published) - set(expected):
        raise ValueError("release contains unexpected or duplicate assets")
    with tempfile.TemporaryDirectory(prefix="kfaceauth-release-readback-") as directory:
        download = Path(directory)
        for name in published:
            run("gh", "release", "download", tag, "--repo", repository, "--pattern", name, "--dir", str(download))
            if (download / name).read_bytes() != expected[name].read_bytes():
                raise ValueError(f"published asset differs; refusing replacement: {name}")
        for name in sorted(set(expected) - set(published)):
            run("gh", "release", "upload", tag, str(expected[name]), "--repo", repository)
        readback = download / "complete"
        readback.mkdir()
        run("gh", "release", "download", tag, "--repo", repository, "--dir", str(readback))
        if {path.name for path in readback.iterdir()} != set(expected):
            raise ValueError("published artifact set differs after upload")
        for name, original in expected.items():
            if (readback / name).read_bytes() != original.read_bytes():
                raise ValueError(f"published readback differs: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        tag, repository = os.environ["TAG_NAME"], os.environ["GITHUB_REPOSITORY"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("invalid release repository")
        upload(args.root, args.artifacts.resolve(), tag, repository)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(1, f"Release upload rejected: {error}\n")
    print("Release artifacts match the tagged source and published readback.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
