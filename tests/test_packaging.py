from __future__ import annotations

import re
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IDENTITY = ROOT / "cmake/ProjectIdentity.cmake"
SPEC = ROOT / "packaging/fedora/kfaceauth.spec"
VERSION = re.search(r'set\(KFACEAUTH_VERSION "([0-9.]+)"\)', IDENTITY.read_text()).group(1)


class PackagingContractTests(unittest.TestCase):
    def test_central_identity_drives_project_metadata(self) -> None:
        identity = IDENTITY.read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        metadata = (ROOT / "src/kcm/kcm_kfaceauth.json.in").read_text(
            encoding="utf-8"
        )
        desktop = (ROOT / "data/kcm_kfaceauth.desktop.in").read_text(
            encoding="utf-8"
        )
        spec = SPEC.read_text(encoding="utf-8")

        for declaration in (
            'set(KFACEAUTH_PROJECT_ID "kfaceauth")',
            f'set(KFACEAUTH_VERSION "{VERSION}")',
            'set(KFACEAUTH_DISPLAY_NAME "LoofiFace-ID")',
            'set(KFACEAUTH_KCM_ID "kcm_kfaceauth")',
            'set(KFACEAUTH_APP_ID "io.github.loofiboss_bit.KFaceAuth")',
            'set(KFACEAUTH_I18N_DOMAIN "kcm_kfaceauth")',
            'set(KFACEAUTH_PREVIEW_WORKER "kfaceauth-camera-preview-worker")',
            'set(KFACEAUTH_VISION_WORKER "kfaceauth-vision-worker")',
            'set(KFACEAUTH_IDENTITY_WORKER "kfaceauth-identity-worker")',
            'set(KFACEAUTH_MODEL_DIRECTORY "kfaceauth/models")',
            'set(KFACEAUTH_MODEL_MANIFEST "manifest.kfaceauth")',
        ):
            self.assertIn(declaration, identity)

        self.assertIn(
            "project(${KFACEAUTH_PROJECT_ID} VERSION ${KFACEAUTH_VERSION}",
            cmake,
        )
        self.assertIn(
            '"Name": "@KFACEAUTH_DISPLAY_NAME@"',
            metadata,
        )
        self.assertIn("Name=@KFACEAUTH_DISPLAY_NAME@", desktop)
        self.assertIn('"Version": "@PROJECT_VERSION@"', metadata)
        self.assertIn("Exec=systemsettings @KFACEAUTH_KCM_ID@", desktop)
        self.assertRegex(spec, r"(?m)^Name:\s+kfaceauth$")
        self.assertRegex(spec, rf"(?m)^Version:\s+{re.escape(VERSION)}$")
        self.assertRegex(spec, r"(?m)^Release:\s+[1-9][0-9]*")
        self.assertRegex(spec, r"(?m)^URL:\s+https://github\.com/loofiboss-bit/LoofiFaceID$")
        self.assertRegex(
            spec,
            r"(?m)^Source0:\s+%\{url\}/releases/download/v%\{version\}/"
            r"%\{name\}-%\{version\}\.tar\.gz$",
        )

    def test_spec_has_no_external_engine_or_privileged_runtime_dependency(self) -> None:
        spec = SPEC.read_text(encoding="utf-8")
        legacy_package = "plasma-" + "irlume"

        self.assertEqual(spec.lower().count(legacy_package), 2)
        self.assertRegex(
            spec,
            r"(?m)^Obsoletes:\s+plasma-irlume < 5\.1\.0$",
        )
        self.assertRegex(
            spec,
            r"(?m)^Provides:\s+plasma-irlume = %\{version\}-%\{release\}$",
        )
        main_metadata = spec.split("%package experimental-auth", 1)[0]
        standard_build_requirements = main_metadata.split(
            "%if %{with experimental_auth}", 1
        )[0]
        self.assertRegex(
            standard_build_requirements,
            r"(?m)^BuildRequires:\s+systemd-devel$",
        )
        self.assertNotRegex(main_metadata, r"(?im)^Requires:.*(?:face|biometric|pam)")
        self.assertNotRegex(spec, r"(?m)^%(?:pre|post|preun|postun|trigger)(?:\s|$)")
        self.assertNotIn("kf6-kauth", spec)
        self.assertIn("%{_libexecdir}/kfaceauth-camera-preview-worker", spec)
        self.assertIn("%{_libexecdir}/kfaceauth-vision-worker", spec)
        self.assertIn("%{_libexecdir}/kfaceauth-identity-worker", spec)
        self.assertIn("face_detection_yunet_2023mar.onnx", spec)
        self.assertIn("face_recognition_sface_2021dec.onnx", spec)
        for directory in (
            "%dir %{_datadir}/kfaceauth",
            "%dir %{_datadir}/kfaceauth/models",
            "%dir %{_datadir}/kfaceauth/models/files",
            "%dir %{_datadir}/kfaceauth/models/licenses",
            "%dir %{_datadir}/kfaceauth/models/provenance",
        ):
            self.assertIn(directory, spec)
        self.assertNotIn("fake-provider-v1.cfg", spec)
        for dependency in (
            "BuildRequires:  opencv-devel",
            "Requires:       opencv-core",
            "Requires:       opencv-dnn",
            "Requires:       opencv-imgproc",
            "Requires:       opencv-objdetect",
            "BuildRequires:  openssl-devel",
            "BuildRequires:  kf6-kwallet-devel",
            "Requires:       openssl-libs",
            "Requires:       kf6-kwallet",
        ):
            self.assertIn(dependency, spec)
        self.assertRegex(
            spec,
            r"(?m)^License:\s+GPL-3\.0-or-later AND MIT AND Apache-2\.0$",
        )
        self.assertIn("kcm_kfaceauth.so", spec)

    def test_default_package_excludes_system_authentication_artifacts(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        engine_cmake = (ROOT / "engine/CMakeLists.txt").read_text(encoding="utf-8")
        data_cmake = (ROOT / "data/CMakeLists.txt").read_text(encoding="utf-8")
        spec = SPEC.read_text(encoding="utf-8")
        files = spec.split("%files -f kcm_kfaceauth.lang", 1)[1].split(
            "%if %{with experimental_auth}", 1
        )[0]
        experimental_files = spec.split("%files experimental-auth", 1)[1].split(
            "%endif", 1
        )[0]

        self.assertRegex(
            cmake,
            r"(?ms)^option\(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS\n.*?^\s+OFF\n\)",
        )
        self.assertIn("-DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF", spec)
        self.assertIn("-DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=ON", spec)
        self.assertIn("%bcond_with experimental_auth", spec)
        for artifact in (
            "kfaceauthd",
            "kfaceauth-sync-vault",
            "kfaceauth-policy",
            "kfaceauth-camera-config",
            "pam_kfaceauth",
            "kfaceauth.service",
            "kfaceauth.socket",
            "kfaceauth.conf",
            "/selinux/",
        ):
            with self.subTest(artifact=artifact):
                self.assertNotIn(artifact, files)
                self.assertIn(artifact, experimental_files)
        self.assertNotIn("kfaceauth-migrate-vault", files)
        self.assertIn("KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS", engine_cmake)
        self.assertIn("if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)", data_cmake)
        self.assertNotIn("KFACEAUTH_BUILD_SYSTEM_AUTH", cmake + engine_cmake + data_cmake)

    def test_rpm_documentation_is_an_explicit_allowlist(self) -> None:
        spec = SPEC.read_text(encoding="utf-8")
        docs = {
            path
            for line in spec.splitlines()
            if line.startswith("%doc ")
            for path in line.split()[1:]
        }
        self.assertEqual(
            docs,
            {
                "CHANGELOG.md",
                "README.md",
                "docs/ANVANDARGUIDE-SV.md",
                "docs/ARCHITECTURE.md",
                "docs/BUILDING.md",
                "docs/THREAT-BOUNDARY.md",
                "docs/TROUBLESHOOTING.md",
                "docs/USER-GUIDE.md",
            },
        )
        self.assertNotIn("%doc docs/*.md", spec)

    def test_daemon_protocol_has_no_key_export_or_broad_profile_operations(self) -> None:
        daemon = (ROOT / "engine/daemon/src/lib.rs").read_text(encoding="utf-8")
        crypto = (ROOT / "engine/crypto-openssl-sys/native/crypto_bridge.c").read_text(
            encoding="utf-8"
        )
        pam_source = (ROOT / "pam/src/pam_kfaceauth.c").read_text(encoding="utf-8")
        pam_cmake = (ROOT / "pam/CMakeLists.txt").read_text(encoding="utf-8")
        test_cmake = (ROOT / "tests/unit/CMakeLists.txt").read_text(encoding="utf-8")

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
        self.assertIn("kfaceauth_load_master_key_for_uid", crypto)
        self.assertNotIn("KFACEAUTH_TEST_SOCKET_OVERRIDE", pam_cmake)
        self.assertIn("KFACEAUTH_TEST_SOCKET_OVERRIDE=1", test_cmake)
        self.assertIn("#ifdef KFACEAUTH_TEST_SOCKET_OVERRIDE", pam_source)

    def test_source_archive_is_reproducible_in_shape_and_has_no_legacy_identity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / f"kfaceauth-{VERSION}.tar.gz"
            second = Path(directory) / f"kfaceauth-{VERSION}-second.tar.gz"
            subprocess.run(
                [
                    str(ROOT / "packaging/fedora/create-source-archive.sh"),
                    str(output),
                ],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                [
                    str(ROOT / "packaging/fedora/create-source-archive.sh"),
                    str(second),
                ],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertTrue(output.is_file())
            self.assertEqual(output.read_bytes(), second.read_bytes())
            self.assertEqual(output.stat().st_mode & 0o777, 0o644)
            with tarfile.open(output, "r:gz") as archive:
                names = archive.getnames()
            self.assertTrue(names)
            self.assertTrue(
                all(
                    name == f"kfaceauth-{VERSION}"
                    or name.startswith(f"kfaceauth-{VERSION}/")
                    for name in names
                )
            )
            self.assertFalse(
                any("/redhat-linux-build/" in f"/{name}/" for name in names)
            )
            self.assertFalse(any("/.agents/" in f"/{name}/" for name in names))
            for excluded in ("docs/IMPROVEMENT-PLAN.md",):
                self.assertNotIn(f"kfaceauth-{VERSION}/{excluded}", names)
            self.assertIn(
                f"kfaceauth-{VERSION}/docs/RELEASE-ERRATA-V5.0.0.md", names
            )
            legacy_package = "plasma-" + "irlume"
            legacy_names = [name for name in names if legacy_package in name.lower()]
            self.assertEqual(
                legacy_names,
                [
                    f"kfaceauth-{VERSION}/packaging/fedora/tests/"
                    "plasma-irlume-3.0.0-fixture.spec"
                ],
            )

    def test_fedora_44_ci_covers_all_required_checks(self) -> None:
        workflows = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((ROOT / ".github/workflows").glob("*.yml"))
        )

        for required in (
            "fedora:44",
            "clang-format --dry-run --Werror",
            "qmllint",
            "ctest",
            "unittest discover",
            "cargo fmt",
            "cargo clippy",
            "cargo test",
            "--offline",
            "verify_models.py",
            "rpmbuild -ba",
            "rpm-smoke-test.sh",
            "kfaceauth-[0-9]*.rpm",
            "kfaceauth-fedora-44",
            "collect-release-artifacts.sh",
            "upload_release_artifacts.py",
            "retention-days: 7",
            "actions/download-artifact@",
            "KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=${{ matrix.experimental_auth }}",
            "experimental_auth: OFF",
            "experimental_auth: ON",
            "configuration: experimental-auth",
            "verify-staged-payload.sh",
            "verify-rpm-payload.sh",
            "--with experimental_auth",
        ):
            with self.subTest(required=required):
                self.assertIn(required, workflows)

        action_refs = re.findall(r"uses:\s+\S+@([0-9a-f]+)", workflows)
        self.assertTrue(action_refs)
        self.assertTrue(all(len(ref) == 40 for ref in action_refs))

    def test_current_document_links_resolve(self) -> None:
        documents = (
            ROOT / "README.md",
            ROOT / "docs/BUILDING.md",
            ROOT / "docs/HARDWARE-QUALIFICATION.md",
            ROOT / "docs/RELEASE-CHECKLIST.md",
            ROOT / "docs/RELEASE-QUALIFICATION-V5.2.md",
            ROOT / "docs/ROADMAP.md",
            ROOT / "docs/TEST-MATRIX.md",
        )
        link_pattern = re.compile(r"\[[^\]]+\]\(([^)\s]+)(?:\s+[^)]*)?\)")
        broken: list[tuple[str, str]] = []
        for document in documents:
            for target in link_pattern.findall(document.read_text(encoding="utf-8")):
                if "://" in target or target.startswith(("mailto:", "#")):
                    continue
                path = target.split("#", maxsplit=1)[0]
                if path and not (document.parent / path).exists():
                    broken.append((document.relative_to(ROOT).as_posix(), target))
        self.assertEqual(broken, [])

    def test_release_workflow_uses_least_privilege_and_complete_artifacts(
        self,
    ) -> None:
        workflow = (ROOT / ".github/workflows/rpm.yml").read_text(encoding="utf-8")

        self.assertRegex(workflow, r"(?m)^permissions:\n  contents: read$")
        self.assertRegex(
            workflow,
            r"(?ms)^  rpm:\n    permissions:\n      contents: read\n",
        )
        self.assertRegex(
            workflow,
            r"(?ms)^  release-upload:\n"
            r"    if: >-\n"
            r"      \(github\.event_name == 'release' "
            r"&& github\.event\.action == 'published'\) \|\|\n"
            r"      \(github\.event_name == 'workflow_dispatch' "
            r"&& inputs\.release_tag != ''\)\n"
            r"    needs: \[rpm, experimental-auth-rpm\]\n"
            r"    permissions:\n"
            r"      actions: read\n"
            r"      contents: write\n",
        )
        self.assertEqual(workflow.count("contents: write"), 1)
        self.assertIn("tools/verify_project_identity.py --field archive", workflow)
        self.assertIn("retention-days: 7", workflow)
        self.assertIn("python3 tools/upload_release_artifacts.py artifacts", workflow)
        self.assertNotIn("--clobber", workflow)
        self.assertIn(
            "TAG_NAME: ${{ inputs.release_tag || github.event.release.tag_name }}",
            workflow,
        )
        self.assertNotIn("if [ -n \"$TAG_NAME\" ]", workflow)
        experimental_job = workflow.split("  experimental-auth-rpm:\n", 1)[1]
        self.assertNotIn("actions/upload-artifact@", experimental_job)

        collector = ROOT / "packaging/fedora/collect-release-artifacts.sh"
        verifier = ROOT / "packaging/fedora/verify-release-artifacts.sh"
        payload_verifier = ROOT / "packaging/fedora/verify-rpm-payload.sh"
        staged_verifier = ROOT / "packaging/fedora/verify-staged-payload.sh"
        self.assertTrue(collector.stat().st_mode & 0o111)
        self.assertTrue(verifier.stat().st_mode & 0o111)
        self.assertTrue(payload_verifier.stat().st_mode & 0o111)
        self.assertTrue(staged_verifier.stat().st_mode & 0o111)
        collector_text = collector.read_text(encoding="utf-8")
        verifier_text = verifier.read_text(encoding="utf-8")
        for required in (
            "tools/verify_project_identity.py",
            "Expected exactly one binary",
            "Expected exactly one non-empty kfaceauth ${version} source RPM",
            "sha256sum --",
        ):
            self.assertIn(required, collector_text)
        for required in (
            "Expected exactly four release files",
            "SHA256SUMS must contain each release payload exactly once",
            "sha256sum --check --strict SHA256SUMS",
        ):
            self.assertIn(required, verifier_text)

    def test_transition_fixture_and_installed_troubleshooting_are_bounded(
        self,
    ) -> None:
        smoke = (ROOT / "packaging/fedora/rpm-smoke-test.sh").read_text(
            encoding="utf-8"
        )
        fixture = (
            ROOT
            / "packaging/fedora/tests/plasma-irlume-3.0.0-fixture.spec"
        ).read_text(encoding="utf-8")
        troubleshooting = (ROOT / "docs/TROUBLESHOOTING.md").read_text(
            encoding="utf-8"
        )
        qualification = (ROOT / "docs/V4-QUALIFICATION-REPORT.md").read_text(
            encoding="utf-8"
        )

        self.assertIn("rpmbuild -bb", smoke)
        self.assertIn("Legacy plasma-irlume payload remains after upgrade", smoke)
        self.assertIn("plasma-irlume.conf", smoke)
        self.assertNotIn("BuildArch:", fixture)
        self.assertNotRegex(fixture, r"(?m)^%(?:pre|post|preun|postun|trigger)")
        self.assertNotIn("/usr/share/doc/kfaceauth/tools/verify_models.py", troubleshooting)
        self.assertIn("rpm -V kfaceauth", troubleshooting)
        for field in (
            "Qualification date",
            "Tested commit",
            "Camera class",
            "Consent scope",
            "Correct-person aggregate result categories",
            "Consenting wrong-person aggregate result categories",
            "Cold latency",
            "Peak resident memory",
            "Remaining release blockers",
        ):
            self.assertIn(field, qualification)
        self.assertIn("UNQUALIFIED", qualification)
        self.assertIn("Never store", qualification)


if __name__ == "__main__":
    unittest.main()
