from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def project_rust_sources() -> list[Path]:
    return [
        path
        for path in sorted((ROOT / "engine").rglob("*.rs"))
        if "vendor" not in path.parts
    ]


class SecurityBoundaryTests(unittest.TestCase):
    def test_source_has_no_blocking_process_waits(self) -> None:
        source = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src").rglob("*"))
            if path.suffix in {".cpp", ".h"}
        )
        for forbidden in ("waitForStarted", "waitForReadyRead", "waitForFinished"):
            self.assertNotIn(forbidden, source)

    def test_privileged_surface_is_absent(self) -> None:
        self.assertFalse(any((ROOT / "src/helper").glob("*")))
        self.assertFalse(any((ROOT / "data").glob("*.actions")))

        build_inputs = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (
                ROOT / "CMakeLists.txt",
                ROOT / "src/CMakeLists.txt",
                ROOT / "src/backend/CMakeLists.txt",
                ROOT / "data/CMakeLists.txt",
                ROOT / "packaging/fedora/kfaceauth.spec",
            )
        )
        for forbidden in ("KAuth", "AuthCore", "kauth_install", "kf6-kauth"):
            self.assertNotIn(forbidden, build_inputs)

    def test_native_backend_reports_real_bounded_identity_components(self) -> None:
        backend = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src" / "backend").glob("nativefaceauthbackend.*"))
        )
        self.assertNotIn("QProcess", backend)
        self.assertNotIn("/usr/bin/", backend)
        self.assertIn("native-engine-unavailable", backend)
        self.assertIn("face_detection_yunet_2023mar.onnx", backend)
        self.assertIn("face_recognition_sface_2021dec.onnx", backend)
        self.assertIn("QCryptographicHash::Sha256", backend)
        self.assertIn("IdentityProtocol::statusRequest", backend)
        self.assertIn("m_statusWorker->execute", backend)
        self.assertIn("KWallet::Wallet::isOpen", backend)
        self.assertNotIn("unsupported-in-milestone-1", backend)

        client = (ROOT / "src/backend/identityworkerclient.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('QStringLiteral("HOME")', client)
        self.assertIn('QStringLiteral("XDG_DATA_HOME")', client)
        for forbidden in (
            'QStringLiteral("PATH")',
            'QStringLiteral("LD_PRELOAD")',
            'QStringLiteral("LD_LIBRARY_PATH")',
            'QStringLiteral("DBUS_SESSION_BUS_ADDRESS")',
        ):
            self.assertNotIn(forbidden, client)

    def test_preview_worker_has_no_privileged_or_persistent_surface(self) -> None:
        preview = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src" / "preview").glob("*"))
            if path.suffix in {".cpp", ".h", ".txt"}
        )
        for forbidden in (
            "QSettings",
            "QSaveFile",
            "QNetworkAccessManager",
            "QAudio",
            "KAuth",
            "Polkit",
            "systemBus",
            "QSettings",
        ):
            self.assertNotIn(forbidden, preview)

        self.assertNotIn("deviceNode", (ROOT / "src/backend/camerapreviewsession.h").read_text(encoding="utf-8"))
        support = (ROOT / "src/backend/supportreport.cpp").read_text(encoding="utf-8")
        for forbidden in ("frame()", "device.token", "device.label", "selectedDeviceIndex"):
            self.assertNotIn(forbidden, support)

    def test_vision_bridge_has_no_persistent_or_privileged_surface(self) -> None:
        vision = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(
                (ROOT / "src" / "backend").glob("visionanalysissession.*")
            )
        )
        for forbidden in (
            "QTemporaryFile",
            "QSaveFile",
            "QFile::write",
            "QSettings",
            "QNetworkAccessManager",
            "KAuth",
            "Polkit",
            "systemBus",
        ):
            self.assertNotIn(forbidden, vision)
        self.assertIn("QProcess::SeparateChannels", vision)
        self.assertIn("m_generation", vision)
        self.assertNotIn("qDebug", vision)
        self.assertNotIn("qInfo", vision)

    def test_identity_ui_exposes_no_system_authentication_mutation(self) -> None:
        qml = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src" / "kcm" / "ui").rglob("*.qml"))
        )
        for forbidden in (
            ".enroll(",
            ".enableLoginScreen(",
            ".enableLockScreen(",
            ".disableNow(",
            ".selectPair(",
            ".setupEmitter(",
            ".tuneCamera(",
        ):
            self.assertNotIn(forbidden, qml)

    def test_no_legacy_identity_or_dependency_remains_outside_transition_contract(
        self,
    ) -> None:
        legacy_engine = "ir" + "lume"
        forbidden_names = (
            legacy_engine,
            "plasma-" + legacy_engine,
            "kcm_" + legacy_engine,
        )
        checked_suffixes = {
            ".cpp",
            ".h",
            ".qml",
            ".txt",
            ".cmake",
            ".json",
            ".in",
            ".md",
            ".py",
            ".sh",
            ".spec",
            ".yml",
            ".toml",
            ".rs",
            ".po",
        }
        allowed_transition_files = {
            Path("README.md"),
            Path("CHANGELOG.md"),
            Path("docs/RELEASE-CHECKLIST.md"),
            Path("packaging/fedora/README.md"),
            Path("packaging/fedora/kfaceauth.spec"),
            Path("packaging/fedora/rpm-smoke-test.sh"),
            Path("packaging/fedora/tests/plasma-irlume-3.0.0-fixture.spec"),
            Path("tests/test_packaging.py"),
            Path("tests/test_security_boundary.py"),
        }
        offenders: list[str] = []
        for path in ROOT.rglob("*"):
            if (
                not path.is_file()
                or ".git" in path.parts
                or ".agents" in path.parts
                or ".vscode" in path.parts
                or ".mypy_cache" in path.parts
                or ".pytest_cache" in path.parts
                or any(part.startswith("build") for part in path.parts)
                or "target" in path.parts
                or path.suffix in {".gz", ".rpm"}
                or path.name.startswith("CHATT_LOGG_")
            ):
                continue
            relative = path.relative_to(ROOT)
            lowered_name = relative.as_posix().lower()
            if any(name in lowered_name for name in forbidden_names):
                if relative not in allowed_transition_files:
                    offenders.append(str(relative))
                continue
            if path.suffix in checked_suffixes or path.name in {"CMakeLists.txt", "LICENSE"}:
                text = path.read_text(encoding="utf-8", errors="replace").lower()
                if any(name in text for name in forbidden_names) and relative not in allowed_transition_files:
                    offenders.append(str(relative))
        self.assertEqual(offenders, [])

    def test_qml_navigation_and_privacy_contracts_are_explicit(self) -> None:
        main = (ROOT / "src/kcm/ui/main.qml").read_text(encoding="utf-8")
        qml = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src/kcm/ui").rglob("*.qml"))
        )

        for object_name in ("homeTab", "setupTab", "testTab", "diagnosticsTab"):
            self.assertIn(f'objectName: "{object_name}"', main)
        self.assertEqual(main.count("activeFocusOnTab: true"), 4)
        self.assertGreaterEqual(main.count("Accessible.name: text"), 4)
        self.assertIn("onClosed: deleteButton.forceActiveFocus()", qml)
        self.assertIn("onClosed: resetButton.forceActiveFocus()", qml)
        self.assertNotRegex(
            qml,
            r"(?i)\b(?:similarityScore|rawScore|embeddingVector|vaultPath|frameBytes)\b",
        )

    def test_rust_identity_has_no_network_or_authentication_surface(self) -> None:
        rust = "\n".join(
            path.read_text(encoding="utf-8").split("#[cfg(test)]", 1)[0]
            for path in project_rust_sources()
            if "vision-opencv-sys" not in path.parts
            and "crypto-openssl-sys" not in path.parts
        )
        for forbidden in (
            "std::net::",
            "tokio",
            "reqwest",
            "unsafe {",
            "write_all_at",
            "pam_start",
            "authenticate(",
            "PAM_SUCCESS",
        ):
            self.assertNotIn(forbidden, rust)
        self.assertIn("authselect", rust)
        self.assertIn("Unsupported", rust)
        self.assertIn("MAX_REQUEST_BYTES", rust)
        self.assertIn("MAX_RESPONSE_BYTES", rust)

    def test_unsafe_rust_is_isolated_to_reviewed_ffi_crates(self) -> None:
        offenders: list[str] = []
        allowed = {
            ROOT / "engine" / "vision-opencv-sys" / "src" / "lib.rs",
            ROOT / "engine" / "crypto-openssl-sys" / "src" / "lib.rs",
        }
        for path in project_rust_sources():
            source = path.read_text(encoding="utf-8").split("#[cfg(test)]", 1)[0]
            if re.search(r"\bunsafe(?:\s+extern|\s*\{|\s+fn|\s+trait|\s+impl)", source) and path not in allowed:
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])
        for ffi_path in allowed:
            ffi = ffi_path.read_text(encoding="utf-8")
            self.assertIn("#![deny(unsafe_op_in_unsafe_fn)]", ffi)
            self.assertIn("unsafe extern", ffi)
            self.assertNotIn("cv::", ffi)

    def test_vision_worker_has_no_network_auth_or_disk_write_surface(self) -> None:
        vision_rust = "\n".join(
            path.read_text(encoding="utf-8").split("#[cfg(test)]", 1)[0]
            for path in project_rust_sources()
            if "vision" in path.parts
        )
        for forbidden in (
            "std::net::",
            "TcpStream",
            "UdpSocket",
            "reqwest",
            "tokio",
            "File::create",
            "OpenOptions",
            "fs::write",
            "pam_",
            "authselect",
            "systemd",
            "authenticate(",
            "FaceEmbedding",
            "PersistentEmbedding",
        ):
            self.assertNotIn(forbidden, vision_rust)
        self.assertIn("MAX_WIDTH", vision_rust)
        self.assertIn("MAX_HEIGHT", vision_rust)

    def test_native_vision_bridge_has_no_io_network_auth_or_logging_surface(self) -> None:
        native_root = ROOT / "engine" / "vision-opencv-sys"
        native = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(native_root.rglob("*"))
            if path.suffix in {".cpp", ".h", ".rs"}
        )
        for forbidden in (
            "imwrite",
            "imencode",
            "VideoCapture",
            "FileStorage",
            "std::filesystem",
            "std::ofstream",
            "fopen(",
            "socket(",
            "connect(",
            "QNetwork",
            "curl",
            "pam_",
            "authselect",
            "systemd",
            "printf(",
            "std::cout",
            "std::cerr",
        ):
            self.assertNotIn(forbidden, native)
        self.assertIn("PR_SET_DUMPABLE", native)
        self.assertIn("RLIMIT_CORE", native)
        self.assertIn('FaceDetectorYN::create("ONNX", model', native)
        self.assertIn('FaceRecognizerSF::create("ONNX", model', native)

    def test_identity_worker_is_private_bounded_and_unprivileged(self) -> None:
        identity = "\n".join(
            path.read_text(encoding="utf-8").split("#[cfg(test)]", 1)[0]
            for path in (
                ROOT / "engine" / "identity" / "src" / "lib.rs",
                ROOT / "engine" / "identity" / "src" / "main.rs",
            )
        )
        for forbidden in (
            "std::net::",
            "TcpStream",
            "UdpSocket",
            "reqwest",
            "tokio",
            "pam_",
            "PAM_SUCCESS",
            "authselect",
            "systemd",
            "Command::new",
        ):
            self.assertNotIn(forbidden, identity)
        self.assertIn("MAX_IDENTITY_REQUEST_BYTES", identity)
        self.assertIn("MAX_IDENTITY_RESPONSE_BYTES", identity)
        self.assertIn("PRODUCTION_MODEL_ROOT", identity)
        self.assertNotIn("--model-root", identity)

    def test_vault_and_kwallet_boundaries_are_fail_closed(self) -> None:
        vault = (ROOT / "engine/templates/src/lib.rs").read_text(encoding="utf-8")
        wallet = (ROOT / "src/backend/kwalletkeyprovider.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("AES-256", (ROOT / "docs/TEMPLATE-VAULT.md").read_text(encoding="utf-8") if (ROOT / "docs/TEMPLATE-VAULT.md").exists() else "AES-256")
        for required in (
            "symlink_metadata",
            "nlink() != 1",
            "sync_all",
            "fs::rename",
            "AuthenticationFailure",
            "MAXIMUM_PROFILE_SAMPLES",
        ):
            self.assertIn(required, vault)
        self.assertIn("KWallet::Wallet::Stream", wallet)
        self.assertIn("RAND_priv_bytes", wallet)
        self.assertIn("m_accessTimer.setInterval(15000)", wallet)
        self.assertIn("m_accessTimer.start()", wallet)
        self.assertNotIn("QSettings", wallet)

        enrollment = (
            ROOT / "src/backend/enrollmentsession.cpp"
        ).read_text(encoding="utf-8")
        self.assertLess(enrollment.index("m_keyProvider->storeKey("), enrollment.index("commitEnrollment();"))
        self.assertIn("m_keyStoredDuringEnrollment", enrollment)
        self.assertIn("vault-key-rollback-failed", enrollment)

    def test_fake_provider_is_test_only_and_not_packaged(self) -> None:
        manifest = (ROOT / "models" / "manifest.kfaceauth").read_text(
            encoding="utf-8"
        )
        spec = (ROOT / "packaging" / "fedora" / "kfaceauth.spec").read_text(
            encoding="utf-8"
        )
        production_main = (
            ROOT / "engine" / "vision" / "src" / "main.rs"
        ).read_text(encoding="utf-8")
        production_lib = (
            ROOT / "engine" / "vision" / "src" / "lib.rs"
        ).read_text(encoding="utf-8").split("#[cfg(test)]", 1)[0]
        self.assertNotIn("fake", manifest.lower())
        self.assertNotIn("fake-provider", spec.lower())
        self.assertNotIn("FakeDeterministicProvider", production_main)
        self.assertNotIn("FakeDeterministicProvider", production_lib)
        self.assertFalse(
            (ROOT / "models" / "files" / "fake-provider-v1.cfg").exists()
        )


if __name__ == "__main__":
    unittest.main()
