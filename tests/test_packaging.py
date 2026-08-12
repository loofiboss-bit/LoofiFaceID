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
            'set(KFACEAUTH_VERSION "4.0.0")',
            'set(KFACEAUTH_DISPLAY_NAME "KFaceAuth")',
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
            '"Name": "@KFACEAUTH_DISPLAY_NAME@ (Experimental Local Identity Preview)"',
            metadata,
        )
        self.assertIn('"Version": "@PROJECT_VERSION@"', metadata)
        self.assertIn("Exec=systemsettings @KFACEAUTH_KCM_ID@", desktop)
        self.assertRegex(spec, r"(?m)^Name:\s+kfaceauth$")
        self.assertRegex(spec, r"(?m)^Version:\s+4\.0\.0$")
        self.assertRegex(spec, r"(?m)^Release:\s+1")
        self.assertRegex(spec, r"(?m)^URL:\s+https://github\.com/loofiboss-bit/plasma-irlume$")
        self.assertRegex(
            spec,
            r"(?m)^Source0:\s+%\{url\}/releases/download/v%\{version\}/"
            r"%\{name\}-%\{version\}\.tar\.gz$",
        )

    def test_spec_has_no_external_engine_or_privileged_runtime_dependency(self) -> None:
        spec = SPEC.read_text(encoding="utf-8")
        legacy_package = "plasma-" + "irlume"

        self.assertEqual(spec.lower().count(legacy_package), 3)
        self.assertRegex(
            spec,
            r"(?m)^Obsoletes:\s+plasma-irlume < 4\.0\.0$",
        )
        self.assertRegex(
            spec,
            r"(?m)^Provides:\s+plasma-irlume = %\{version\}-%\{release\}$",
        )
        self.assertNotRegex(spec, r"(?im)^Requires:.*(?:face|biometric|pam)")
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

    def test_source_archive_is_reproducible_in_shape_and_has_no_legacy_identity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "kfaceauth-4.0.0.tar.gz"
            second = Path(directory) / "kfaceauth-4.0.0-second.tar.gz"
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
                    name == "kfaceauth-4.0.0"
                    or name.startswith("kfaceauth-4.0.0/")
                    for name in names
                )
            )
            self.assertFalse(
                any("/redhat-linux-build/" in f"/{name}/" for name in names)
            )
            legacy_package = "plasma-" + "irlume"
            legacy_names = [name for name in names if legacy_package in name.lower()]
            self.assertEqual(
                legacy_names,
                [
                    "kfaceauth-4.0.0/packaging/fedora/tests/"
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
            "verify-release-artifacts.sh",
            "retention-days: 7",
            "actions/download-artifact@",
        ):
            with self.subTest(required=required):
                self.assertIn(required, workflows)

        action_refs = re.findall(r"uses:\s+\S+@([0-9a-f]+)", workflows)
        self.assertTrue(action_refs)
        self.assertTrue(all(len(ref) == 40 for ref in action_refs))

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
            r"    if: github\.event_name == 'release' "
            r"&& github\.event\.action == 'published'\n"
            r"    needs: rpm\n"
            r"    permissions:\n"
            r"      actions: read\n"
            r"      contents: write\n",
        )
        self.assertEqual(workflow.count("contents: write"), 1)
        self.assertIn('"$PWD/kfaceauth-4.0.0.tar.gz"', workflow)
        self.assertIn("retention-days: 7", workflow)
        self.assertIn("gh release upload", workflow)
        self.assertNotIn("if [ -n \"$TAG_NAME\" ]", workflow)

        collector = ROOT / "packaging/fedora/collect-release-artifacts.sh"
        verifier = ROOT / "packaging/fedora/verify-release-artifacts.sh"
        self.assertTrue(collector.stat().st_mode & 0o111)
        self.assertTrue(verifier.stat().st_mode & 0o111)
        collector_text = collector.read_text(encoding="utf-8")
        verifier_text = verifier.read_text(encoding="utf-8")
        for required in (
            "kfaceauth-4.0.0.tar.gz",
            "Expected exactly one binary",
            "Expected exactly one non-empty kfaceauth 4.0.0 source RPM",
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
