from __future__ import annotations

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExperimentalAuthBoundaryTests(unittest.TestCase):
    def test_default_build_and_base_rpm_exclude_system_authentication(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        data_cmake = (ROOT / "data/CMakeLists.txt").read_text(encoding="utf-8")
        engine_cmake = (ROOT / "engine/CMakeLists.txt").read_text(encoding="utf-8")
        spec = (ROOT / "packaging/fedora/kfaceauth.spec").read_text(encoding="utf-8")
        release_status = (ROOT / "docs/RELEASE-QUALIFICATION-V5.1.md").read_text(
            encoding="utf-8"
        )

        self.assertRegex(
            cmake,
            r"(?ms)^option\(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS\n.*?^\s+OFF\n\)",
        )
        self.assertIn("if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)", cmake)
        self.assertIn("add_subdirectory(data)", cmake)
        self.assertIn("if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)", data_cmake)
        self.assertIn("kcm_kfaceauth.desktop.in", data_cmake)
        self.assertLess(
            data_cmake.index("kcm_kfaceauth.desktop.in"),
            data_cmake.index("if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)"),
        )
        self.assertIn("systemd/kfaceauth.service", data_cmake)
        self.assertIn("-DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF", spec)
        self.assertIn("if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)", engine_cmake)
        self.assertIn("Face unlock is not shipped", release_status)

        files = spec.split("%files -f kcm_kfaceauth.lang", 1)[1].split(
            "%changelog", 1
        )[0]
        for artifact in (
            "kfaceauthd",
            "pam_kfaceauth",
            "kfaceauth.service",
            "kfaceauth.socket",
            "kfaceauth.conf",
            "kfaceauth-migrate-vault",
        ):
            with self.subTest(artifact=artifact):
                self.assertNotIn(artifact, files)

    def test_daemon_requests_are_uid_bound_and_narrow(self) -> None:
        daemon = (ROOT / "engine/daemon/src/lib.rs").read_text(encoding="utf-8")

        self.assertIn("peer_uid == target_uid", daemon)
        self.assertNotIn("peer_uid == 0 ||", daemon)
        for removed in (
            "OP_GET_KEY",
            "OP_VERIFY_FRAME",
            "OP_DELETE_PROFILE",
            "GetKey",
            "VerifyFrame",
            "DeleteProfile",
        ):
            with self.subTest(removed=removed):
                self.assertNotIn(removed, daemon)
        self.assertIn("removed_key_export_and_broad_profile_operations_are_rejected", daemon)

    def test_status_key_load_does_not_create_secret_material(self) -> None:
        bridge = (ROOT / "engine/crypto-openssl-sys/native/crypto_bridge.c").read_text(
            encoding="utf-8"
        )
        crypto_api = (ROOT / "engine/crypto-openssl-sys/src/lib.rs").read_text(
            encoding="utf-8"
        )
        daemon = (ROOT / "engine/daemon/src/lib.rs").read_text(encoding="utf-8")
        loader = bridge.split(
            "int kfaceauth_load_master_key_for_uid", 1
        )[1].split("int kfaceauth_seal_master_key", 1)[0]

        self.assertNotIn("ensure_dir_exists", loader)
        self.assertNotIn("kfaceauth_crypto_random", loader)
        self.assertNotIn("write_key_file", loader)
        self.assertIn("load_master_key_for_uid", crypto_api)
        self.assertIn("load_master_key_for_uid", daemon)
        self.assertIn("master_key_load_requires_explicit_provisioning", crypto_api)

    def test_production_pam_uses_fixed_socket_and_test_override_is_test_only(self) -> None:
        pam_source = (ROOT / "pam/src/pam_kfaceauth.c").read_text(encoding="utf-8")
        pam_cmake = (ROOT / "pam/CMakeLists.txt").read_text(encoding="utf-8")
        unit_cmake = (ROOT / "tests/unit/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("#ifdef KFACEAUTH_TEST_SOCKET_OVERRIDE", pam_source)
        self.assertIn('const char *sock_path = DEFAULT_SOCKET_PATH;', pam_source)
        self.assertNotIn("KFACEAUTH_TEST_SOCKET_OVERRIDE", pam_cmake)
        self.assertIn("KFACEAUTH_TEST_SOCKET_OVERRIDE=1", unit_cmake)
        self.assertFalse(
            (ROOT / "engine/templates/src/bin/kfaceauth-migrate-vault.rs").exists()
        )


if __name__ == "__main__":
    unittest.main()
