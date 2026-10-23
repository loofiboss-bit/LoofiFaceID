#!/usr/bin/env python3
"""Read canonical identity and reject inconsistent build metadata offline."""
from __future__ import annotations

import argparse
import json
import re
import tomllib
from pathlib import Path


IDENTITY_PATTERNS = (
    ("name", re.compile(r'^set\(KFACEAUTH_PROJECT_ID "([a-z][a-z0-9-]*)"\)$', re.MULTILINE)),
    ("version", re.compile(r'^set\(KFACEAUTH_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\)$', re.MULTILINE)),
)

RPM_PATTERNS = (
    ("Name", "name", re.compile(r"^Name:\s+(\S+)\s*$", re.MULTILINE)),
    ("Version", "version", re.compile(r"^Version:\s+(\S+)\s*$", re.MULTILINE)),
)


def identity(root: Path) -> dict[str, str]:
    text = (root / "cmake/ProjectIdentity.cmake").read_text(encoding="utf-8")
    result = {}
    for field, pattern in IDENTITY_PATTERNS:
        matches = pattern.findall(text)
        if len(matches) != 1:
            raise ValueError(f"invalid canonical {field}")
        result[field] = matches[0]
    result["archive"] = f"{result['name']}-{result['version']}.tar.gz"
    return result


def verify(root: Path, values: dict[str, str]) -> None:
    cargo = tomllib.loads((root / "engine/Cargo.toml").read_text(encoding="utf-8"))
    if cargo["workspace"]["package"]["version"] != values["version"]:
        raise ValueError("Cargo version differs from canonical identity")
    spec = (root / "packaging/fedora/kfaceauth.spec").read_text(encoding="utf-8")
    for label, field, pattern in RPM_PATTERNS:
        matches = pattern.findall(spec)
        if matches != [values[field]]:
            raise ValueError(f"RPM {label} differs from canonical identity")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--field", choices=("name", "version", "archive"))
    args = parser.parse_args()
    try:
        values = identity(args.root)
        verify(args.root, values)
    except (OSError, ValueError, KeyError, tomllib.TOMLDecodeError) as error:
        parser.exit(1, f"Project identity verification failed: {error}\n")
    print(values[args.field] if args.field else json.dumps(values, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
