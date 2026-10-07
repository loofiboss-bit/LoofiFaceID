#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check integration markers in a disposable /usr staged install."""

import argparse
import json
from pathlib import Path

SDDM_MARKER = "usr/share/sddm/kfaceauth/sddm-face-auth-api-v1.conf"
LOCK_MARKER = "usr/share/kfaceauth/integrations/kscreenlocker.json"
SDDM_CONTENT = (
    "format=org.loofifaceid.sddm-face-auth\n"
    "api=1\n"
    "sddm_version=0.21.0\n"
    "upstream_commit=63780fcd79f1dbf81a30eef48c28c699ab15aded\n"
)
LOCK_CONTENT = {
    "schema_version": 1,
    "component_id": "org.loofi.kfaceauth.kscreenlocker",
    "kscreenlocker_version": "6.7.5",
    "theme_contract": {
        "interface_version": 1,
        "component_url": "qrc:/fallbacktheme/FaceAuthenticationControl.qml",
        "runtime_registration_required": True,
    },
}


def verify(component: str, configuration: str, stage: Path) -> None:
    if not stage.is_dir() or stage.resolve() == Path("/"):
        raise ValueError("A disposable staging directory is required")
    marker = stage / (SDDM_MARKER if component == "sddm" else LOCK_MARKER)
    candidates = list(stage.rglob("*face-auth-api*.conf")) + list(
        stage.rglob("kfaceauth-sddm-api-v1.conf")
    ) + list(stage.rglob("kscreenlocker.json")) + list(
        stage.rglob("kfaceauth-kscreenlocker.json")
    )
    workers = list(stage.rglob("kscreenlocker_face_pam_worker"))
    if (stage / "etc/pam.d").exists():
        raise ValueError("Integration CI must not stage PAM configuration")
    if configuration == "standard":
        if candidates or workers:
            raise ValueError("The default build installed an experimental marker or worker")
        return
    if candidates != [marker] or marker.is_symlink() or not marker.is_file():
        raise ValueError("Missing, misplaced, duplicate, or symlinked integration marker")
    if component == "sddm":
        if marker.read_bytes() != SDDM_CONTENT.encode("utf-8"):
            raise ValueError("Unexpected SDDM marker contents")
    else:
        installed = json.loads(marker.read_text(encoding="utf-8"))
        # JSON booleans and numbers must remain distinct despite Python equality.
        if json.dumps(installed, sort_keys=True) != json.dumps(LOCK_CONTENT, sort_keys=True):
            raise ValueError("Unexpected KScreenLocker marker contents")
        if len(workers) != 1 or workers[0].is_symlink() or not workers[0].is_file():
            raise ValueError("Expected exactly one installed face PAM worker")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=("sddm", "kscreenlocker"))
    parser.add_argument("configuration", choices=("standard", "experimental"))
    parser.add_argument("stage", type=Path)
    args = parser.parse_args()
    verify(args.component, args.configuration, args.stage)
    print(f"Verified {args.component} {args.configuration} staged install boundary")


if __name__ == "__main__":
    main()
