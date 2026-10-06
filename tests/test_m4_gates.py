from __future__ import annotations

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExperimentalAuthBoundaryTests(unittest.TestCase):
    def test_default_build_and_rpm_exclude_authentication_unless_opted_in(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        data_cmake = (ROOT / "data/CMakeLists.txt").read_text(encoding="utf-8")
        engine_cmake = (ROOT / "engine/CMakeLists.txt").read_text(encoding="utf-8")
        spec = (ROOT / "packaging/fedora/kfaceauth.spec").read_text(encoding="utf-8")
        release_status = (ROOT / "docs/RELEASE-QUALIFICATION-V5.2.md").read_text(
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
        self.assertIn("-DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF", spec)
        self.assertIn("-DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=ON", spec)
        self.assertIn("if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)", engine_cmake)
        self.assertIn("systemd/kfaceauth.service", data_cmake)
        self.assertIn("not supported or qualified for login", release_status)
        self.assertNotIn("KFACEAUTH_BUILD_SYSTEM_AUTH", cmake + data_cmake + engine_cmake)

        files = spec.split("%files -f kcm_kfaceauth.lang", 1)[1].split(
            "%if %{with experimental_auth}", 1
        )[0]
        experimental_files = spec.split("%files experimental-auth", 1)[1].split(
            "%endif", 1
        )[0]
        for artifact in (
            "kfaceauthd",
            "kfaceauth-auth-worker",
            "pam_kfaceauth",
            "kfaceauth.service",
            "kfaceauth.socket",
            "kfaceauth.conf",
            "kfaceauth-sync-vault",
        ):
            with self.subTest(artifact=artifact):
                self.assertNotIn(artifact, files)
                self.assertIn(artifact, experimental_files)
        self.assertNotIn("kfaceauth-migrate-vault", files)

    def test_daemon_requests_are_uid_bound_and_narrow(self) -> None:
        daemon = (ROOT / "engine/daemon/src/lib.rs").read_text(encoding="utf-8")

        self.assertIn("peer_uid == target_uid", daemon)
        self.assertIn("peer_uid == target_uid || peer_uid == 0", daemon)
        self.assertIn("MAX_ACTIVE_CONNECTIONS: usize = 4", daemon)
        self.assertIn("MAX_CONNECTIONS_PER_PEER_UID: usize = 2", daemon)
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

    def test_system_profile_uses_a_separate_key_and_password_fallback(self) -> None:
        helper = (ROOT / "engine/templates/src/bin/kfaceauth-sync-vault.rs").read_text(
            encoding="utf-8"
        )
        vault = (ROOT / "engine/templates/src/lib.rs").read_text(encoding="utf-8")
        pam_source = (ROOT / "pam/src/pam_kfaceauth.c").read_text(encoding="utf-8")
        setup = (ROOT / "data/pam/kfaceauth-pam-setup.sh").read_text(encoding="utf-8")
        sddm_service = (ROOT / "data/pam/sddm-kfaceauth").read_text(encoding="utf-8")
        plasma_service = (ROOT / "data/pam/kde-kfaceauth").read_text(encoding="utf-8")

        self.assertIn("read_exact(&mut key)", helper)
        self.assertIn("migrate_legacy_vault_with_separate_key", helper)
        self.assertIn("MasterKey::generate()", helper)
        self.assertNotIn("--hex-key", helper)
        self.assertNotIn("KFACEAUTH_MASTER_KEY", helper)
        self.assertIn("pre_session_migration_reencrypts_under_a_distinct_key", vault)
        self.assertIn("resp_len != sizeof(resp_body)", pam_source)
        self.assertIn("resp_code == STATUS_SUCCESS", pam_source)
        self.assertIn("continuing with password stack", pam_source)
        self.assertIn("migrate_legacy_global_rules", setup)
        self.assertIn("auth        sufficient    pam_kfaceauth.so", sddm_service)
        self.assertIn("auth        sufficient    pam_kfaceauth.so", plasma_service)
        self.assertIn("--prepare-target", setup)

    def test_blocked_configured_target_remains_disableable(self) -> None:
        header = (ROOT / "src/backend/enrollmentsession.h").read_text(encoding="utf-8")
        auth_page = (ROOT / "src/kcm/ui/AuthIntegrationPage.qml").read_text(encoding="utf-8")

        for target in ("sddm", "plasmaLock"):
            with self.subTest(target=target):
                self.assertIn(f"{target}AuthConfigured", header)
                self.assertIn(f'objectName: "{target}AuthMode"', auth_page)
                self.assertIn('"off", "on-activity", "manual"', auth_page)


if __name__ == "__main__":
    unittest.main()
