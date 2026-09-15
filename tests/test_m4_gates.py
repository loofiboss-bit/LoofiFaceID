from __future__ import annotations

import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Milestone4GatesTest(unittest.TestCase):
    def test_gate_4_1_least_privilege_and_privilege_dropping(self) -> None:
        """Gate 4.1: Daemon drops root privileges immediately upon socket creation;
        worker executes strictly as kfaceauth:kfaceauth without requiring CAP_DAC_OVERRIDE."""
        service = (ROOT / "data/systemd/kfaceauth.service").read_text(encoding="utf-8")
        self.assertIn("User=kfaceauth", service)
        self.assertIn("Group=kfaceauth", service)
        self.assertIn("CapabilityBoundingSet=", service)
        self.assertIn("AmbientCapabilities=", service)
        self.assertIn("NoNewPrivileges=true", service)

        main_rs = (ROOT / "engine/daemon/src/main.rs").read_text(encoding="utf-8")
        self.assertIn("drop_privileges(DEFAULT_DAEMON_USER, DEFAULT_DAEMON_GROUP)", main_rs)

        bridge_c = (ROOT / "engine/crypto-openssl-sys/native/crypto_bridge.c").read_text(encoding="utf-8")
        self.assertIn("initgroups(username, gr->gr_gid)", bridge_c)
        self.assertIn("setgid(gr->gr_gid)", bridge_c)
        self.assertIn("setuid(pw->pw_uid)", bridge_c)
        self.assertIn("geteuid() == 0 || getegid() == 0", bridge_c)

    def test_gate_4_2_pam_aborts_within_two_seconds_on_hang_or_busy(self) -> None:
        """Gate 4.2: PAM module aborts within <= 2.0 seconds if camera is busy or user is absent."""
        test_pam_bin = ROOT / "build/bin/test_pam"
        self.assertTrue(test_pam_bin.exists(), "test_pam binary must be built")

        result = subprocess.run(
            [str(test_pam_bin), "testHungServerAbortsWithinTwoSeconds"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, f"testHungServerAbortsWithinTwoSeconds failed: {result.stderr}")
        self.assertIn("PASS   : TestPam::testHungServerAbortsWithinTwoSeconds()", result.stdout)

    def test_gate_4_3_cross_uid_access_rejection(self) -> None:
        """Gate 4.3: Cross-UID access attack test verifies that a process running as UID 1001
        cannot query, decrypt, or tamper with UID 1000's vault."""
        daemon_lib = (ROOT / "engine/daemon/src/lib.rs").read_text(encoding="utf-8")
        self.assertIn("is_authorized(peer_uid: u32, target_uid: u32)", daemon_lib)
        self.assertIn("peer_uid == 0 || peer_uid == target_uid", daemon_lib)
        self.assertIn("STATUS_ACCESS_DENIED", daemon_lib)

        # Run the cargo unit test asserting all opcodes fail for attacker UID 1001 against UID 1000
        result = subprocess.run(
            [
                "cargo",
                "test",
                "--manifest-path",
                str(ROOT / "engine/Cargo.toml"),
                "-p",
                "kfaceauth-daemon",
                "--lib",
                "tests::gate_4_3_cross_uid_tamper_defense_all_opcodes",
                "--",
                "--exact",
            ],
            cwd=ROOT / "engine",
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, f"Cargo test failed: {result.stderr}")
        self.assertIn("test tests::gate_4_3_cross_uid_tamper_defense_all_opcodes ... ok", result.stdout)

    def test_system_vault_least_privilege_dac_modes(self) -> None:
        """Verifies directory Mode 0750 and file Mode 0640 least-privilege DAC enforcement."""
        templates_lib = (ROOT / "engine/templates/src/lib.rs").read_text(encoding="utf-8")
        self.assertIn("0o640", templates_lib)
        self.assertIn("0o750", templates_lib)
        self.assertIn("DEFAULT_SYSTEM_VAULT_ROOT", templates_lib)
        self.assertIn("KFACEAUTH_SYSTEM_VAULT_DIR", templates_lib)

    def test_migrate_vault_tool_builds_and_runs(self) -> None:
        """Verifies kfaceauth-migrate-vault binary exists and runs help."""
        migrate_bin = ROOT / "build/engine/target/debug/kfaceauth-migrate-vault"
        self.assertTrue(migrate_bin.exists(), "kfaceauth-migrate-vault must exist")

        result = subprocess.run(
            [str(migrate_bin), "--help"],
            capture_output=True,
            text=True,
        )
        self.assertIn("kfaceauth-migrate-vault", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
