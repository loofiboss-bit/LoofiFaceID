#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for integration install boundaries."""

import json
import tempfile
import unittest
from pathlib import Path

from verify_staged_integration import (
    LOCK_CONTENT,
    LOCK_MARKER,
    SDDM_CONTENT,
    SDDM_MARKER,
    verify,
)


class StagedIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.stage = Path(self.temporary.name)

    def write(self, relative: str, content: str) -> Path:
        path = self.stage / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_default_has_no_experimental_payload(self) -> None:
        for component in ("sddm", "kscreenlocker"):
            verify(component, "standard", self.stage)
        self.write(SDDM_MARKER, SDDM_CONTENT)
        with self.assertRaises(ValueError):
            verify("sddm", "standard", self.stage)

    def test_sddm_requires_exact_installed_name_and_content(self) -> None:
        path = self.write(SDDM_MARKER, SDDM_CONTENT)
        verify("sddm", "experimental", self.stage)
        path.write_text(SDDM_CONTENT + "unexpected=true\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify("sddm", "experimental", self.stage)
        path.write_bytes(SDDM_CONTENT.replace("\n", "\r\n").encode("utf-8"))
        with self.assertRaises(ValueError):
            verify("sddm", "experimental", self.stage)
        path.unlink()
        self.write("usr/share/sddm/kfaceauth/kfaceauth-sddm-api-v1.conf", SDDM_CONTENT)
        with self.assertRaises(ValueError):
            verify("sddm", "experimental", self.stage)

    def test_kscreenlocker_requires_worker_and_exact_contract(self) -> None:
        self.write(LOCK_MARKER, json.dumps(LOCK_CONTENT))
        with self.assertRaises(ValueError):
            verify("kscreenlocker", "experimental", self.stage)
        self.write("usr/libexec/kscreenlocker_face_pam_worker", "fixture")
        verify("kscreenlocker", "experimental", self.stage)
        with self.assertRaises(ValueError):
            verify("kscreenlocker", "standard", self.stage)

    def test_kscreenlocker_marker_types_are_strict(self) -> None:
        malformed = {**LOCK_CONTENT, "schema_version": True}
        self.write(LOCK_MARKER, json.dumps(malformed))
        self.write("usr/libexec/kscreenlocker_face_pam_worker", "fixture")
        with self.assertRaises(ValueError):
            verify("kscreenlocker", "experimental", self.stage)

    def test_symlinked_marker_is_rejected(self) -> None:
        source = self.write("other-marker", SDDM_CONTENT)
        path = self.stage / SDDM_MARKER
        path.parent.mkdir(parents=True)
        path.symlink_to(source)
        with self.assertRaises(ValueError):
            verify("sddm", "experimental", self.stage)

    def test_pam_configuration_is_not_staged(self) -> None:
        self.write("etc/pam.d/sddm", "fixture")
        with self.assertRaises(ValueError):
            verify("sddm", "standard", self.stage)


if __name__ == "__main__":
    unittest.main()
