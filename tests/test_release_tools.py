from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import create_source_archive as source
import upload_release_artifacts as release
import verify_project_identity as project


class ReleaseToolTests(unittest.TestCase):
    def test_source_rpm_marker_is_independent_of_build_architecture(self) -> None:
        version = project.identity(ROOT)["version"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts, commands = root / "artifacts", root / "commands"
            artifacts.mkdir()
            commands.mkdir()
            names = (
                f"kfaceauth-{version}.tar.gz",
                f"kfaceauth-{version}-1.fc44.x86_64.rpm",
                f"kfaceauth-{version}-1.fc44.src.rpm",
            )
            for name in names:
                (artifacts / name).write_bytes(b"fixture")
            checksum = hashlib.sha256(b"fixture").hexdigest()
            (artifacts / "SHA256SUMS").write_text(
                "".join(f"{checksum}  ./{name}\n" for name in names)
            )
            rpm = commands / "rpm"
            rpm.write_text(
                f'''#!/bin/sh
case "$4" in
  *.src.rpm) marker=1 ;;
  *) marker='(none)' ;;
esac
case "$3" in
  '%{{NAME}}') echo kfaceauth ;;
  '%{{VERSION}}') echo '{version}' ;;
  '%{{ARCH}}') echo x86_64 ;;
  '%{{SOURCEPACKAGE}}') echo "$marker" ;;
  '%{{NAME}} %{{VERSION}} %{{SOURCEPACKAGE}}') echo "kfaceauth {version} $marker" ;;
  *) exit 1 ;;
esac
'''
            )
            rpm.chmod(0o755)
            environment = dict(os.environ, PATH=f"{commands}:{os.environ['PATH']}")
            command = [
                str(ROOT / "packaging/fedora/verify-release-artifacts.sh"),
                str(artifacts),
            ]
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            rpm.write_text(
                rpm.read_text().replace(
                    "*.src.rpm) marker=1", "*.src.rpm) marker='(none)'"
                )
            )
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Source RPM metadata differs", result.stderr)

    def fixture(self, root: Path) -> None:
        for directory in ("cmake", "engine", "packaging/fedora"):
            (root / directory).mkdir(parents=True, exist_ok=True)
        (root / "cmake/ProjectIdentity.cmake").write_text(
            'set(KFACEAUTH_PROJECT_ID "kfaceauth")\nset(KFACEAUTH_VERSION "9.8.7")\n'
        )
        (root / "engine/Cargo.toml").write_text('[workspace.package]\nversion = "9.8.7"\n')
        (root / "packaging/fedora/kfaceauth.spec").write_text("Name: kfaceauth\nVersion: 9.8.7\n")
        (root / "packaging/fedora/source-files.txt").write_text(
            "cmake/ProjectIdentity.cmake\nengine/Cargo.toml\npackaging/fedora/kfaceauth.spec\npackaging/fedora/source-files.txt\n"
        )

    def test_archive_allowlist_reproducibility_and_gitless_rebuild(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            (root / "private-untracked.txt").write_text("not distributable")
            one, two = root / "one.tar.gz", root / "two.tar.gz"
            source.create_archive(root, one)
            source.create_archive(root, two)
            self.assertEqual(one.read_bytes(), two.read_bytes())
            with tarfile.open(one) as archive:
                self.assertNotIn("kfaceauth-9.8.7/private-untracked.txt", archive.getnames())
                extracted = root / "unpacked"
                archive.extractall(extracted, filter="data")
            source.create_archive(extracted / "kfaceauth-9.8.7", root / "rebuilt.tar.gz")
            self.assertEqual(one.read_bytes(), (root / "rebuilt.tar.gz").read_bytes())

    def test_identity_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            (root / "engine/Cargo.toml").write_text('[workspace.package]\nversion = "1.0.0"\n')
            with self.assertRaises(ValueError):
                source.create_archive(root, root / "out.tar.gz")

    def test_distributable_manifest_tracks_known_source_tree(self):
        entries = source.source_files(ROOT)
        tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True, check=False)
        if tracked.returncode == 0:
            self.assertFalse(set(tracked.stdout.splitlines()) - {"docs/IMPROVEMENT-PLAN.md"} - set(entries))
        self.assertIn("engine/daemon/src/bin/kfaceauth-auth-worker.rs", entries)
        self.assertIn("engine/crypto-openssl-sys/native/camera_selection.h", entries)
        self.assertNotIn("docs/IMPROVEMENT-PLAN.md", entries)
        for entry in entries:
            self.assertFalse(entry.startswith(("build", "LoofiFaceID-", "CHATT_LOGG_")))

    def test_container_workflows_install_git_before_checkout(self):
        ci = (ROOT / ".github/workflows/ci.yml").read_text()
        self.assertLess(ci.index("Install Git before checkout"), ci.index("Check out source"))
        self.assertLess(
            ci.index("Check out source"), ci.index("Configure Git safe directory")
        )
        self.assertIn(
            'run: git config --global --add safe.directory "$GITHUB_WORKSPACE"', ci
        )

        rpm = (ROOT / ".github/workflows/rpm.yml").read_text()
        positions = sorted(
            (rpm.index(f"  {job_name}:"), job_name)
            for job_name in ("rpm", "release-upload", "experimental-auth-rpm")
        )
        for index, (start, job_name) in enumerate(positions):
            end = positions[index + 1][0] if index + 1 < len(positions) else len(rpm)
            job = rpm[start:end]
            install_step = (
                "Install release upload dependencies"
                if job_name == "release-upload"
                else "Install Git before checkout"
            )
            self.assertLess(
                job.index(install_step),
                job.index("Check out source"),
                job_name,
            )
            expected_install = (
                "dnf install -y gh git-core rpm python3"
                if job_name == "release-upload"
                else "dnf install -y git-core"
            )
            self.assertIn(expected_install, job)
            self.assertLess(
                job.index("Check out source"),
                job.index("Configure Git safe directory"),
                job_name,
            )
            self.assertIn(
                'run: git config --global --add safe.directory "$GITHUB_WORKSPACE"',
                job,
            )

    def test_rpm_workflow_can_rebuild_exact_tag_for_release_recovery(self):
        rpm = (ROOT / ".github/workflows/rpm.yml").read_text()
        self.assertIn("release_tag:", rpm)
        self.assertIn("ref: ${{ inputs.release_tag || github.ref }}", rpm)
        self.assertIn("inputs.release_tag || github.event.release.tag_name", rpm)
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.release_tag != ''", rpm)

    def test_manifest_traversal_duplicate_and_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            manifest = root / "packaging/fedora/source-files.txt"
            for entries in ("../private\n", "engine/Cargo.toml\nengine/Cargo.toml\n", "missing\n"):
                manifest.write_text(entries)
                with self.assertRaises(ValueError):
                    source.source_files(root)
            (root / "link").symlink_to(root / "engine/Cargo.toml")
            manifest.write_text("link\n")
            with self.assertRaises(ValueError):
                source.source_files(root)

    def test_source_files_manifest_validation_edge_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            manifest = root / "packaging/fedora/source-files.txt"

            manifest.write_text("")
            with self.assertRaisesRegex(ValueError, "source manifest must be non-empty"):
                source.source_files(root)

            manifest.write_text("a\n" * 10001)
            with self.assertRaisesRegex(ValueError, "source manifest must be non-empty"):
                source.source_files(root)

            manifest.write_text("engine/Cargo.toml\ncmake/ProjectIdentity.cmake\n")
            with self.assertRaisesRegex(ValueError, "source manifest must be non-empty"):
                source.source_files(root)

            manifest.write_text("/etc/passwd\n")
            with self.assertRaisesRegex(ValueError, "unsafe source manifest entry"):
                source.source_files(root)

            manifest.write_text(".git/config\n")
            with self.assertRaisesRegex(ValueError, "unsafe source manifest entry"):
                source.source_files(root)

            manifest.write_text("sub/.git/config\n")
            with self.assertRaisesRegex(ValueError, "unsafe source manifest entry"):
                source.source_files(root)

            for non_canonical in ("./engine/Cargo.toml\n", "engine//Cargo.toml\n", "engine/Cargo.toml/\n"):
                manifest.write_text(non_canonical)
                with self.assertRaisesRegex(ValueError, "unsafe source manifest entry"):
                    source.source_files(root)

            manifest.write_text("nonexistent.txt\n")
            with self.assertRaisesRegex(ValueError, "missing or unsafe source file"):
                source.source_files(root)

            outside_dir = Path(directory) / "outside"
            outside_dir.mkdir()
            outside_file = outside_dir / "secret.txt"
            outside_file.write_text("secret")
            (root / "outside_link").symlink_to(outside_file)
            manifest.write_text("outside_link\n")
            with self.assertRaisesRegex(ValueError, "missing or unsafe source file"):
                source.source_files(root)

    def test_provenance_generation_and_validation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            values = project.identity(root)

            prov_file = root / "SOURCE-PROVENANCE.json"
            valid_prov = {
                "name": "kfaceauth",
                "version": "9.8.7",
                "commit": "1234567890abcdef1234567890abcdef12345678",
                "source_date_epoch": 1600000000,
            }
            prov_file.write_text(json.dumps(valid_prov))
            self.assertEqual(source.provenance(root, values), valid_prov)

            invalid_prov = dict(valid_prov, name="other")
            prov_file.write_text(json.dumps(invalid_prov))
            with self.assertRaisesRegex(ValueError, "source provenance identity mismatch"):
                source.provenance(root, values)

            prov_file.unlink()

            with patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1700000000"}):
                record = source.provenance(root, values)
                self.assertEqual(record["source_date_epoch"], 1700000000)
                self.assertEqual(record["name"], "kfaceauth")
                self.assertEqual(record["version"], "9.8.7")

    def test_create_archive_tar_metadata_and_structure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)

            script = root / "bin/script.sh"
            script.parent.mkdir(parents=True, exist_ok=True)
            script.write_text("#!/bin/sh\necho test\n")
            script.chmod(0o755)

            manifest = root / "packaging/fedora/source-files.txt"
            entries = sorted([
                "bin/script.sh",
                "cmake/ProjectIdentity.cmake",
                "engine/Cargo.toml",
                "packaging/fedora/kfaceauth.spec",
                "packaging/fedora/source-files.txt",
            ])
            manifest.write_text("\n".join(entries) + "\n")

            output = root / "nested/build/output.tar.gz"
            with patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1600000000"}):
                source.create_archive(root, output)

            self.assertTrue(output.is_file())

            with tarfile.open(output, "r:gz") as archive:
                members = {m.name: m for m in archive.getmembers()}
                prefix = "kfaceauth-9.8.7"

                dir_member = members[prefix]
                self.assertTrue(dir_member.isdir())
                self.assertEqual(dir_member.mode, 0o755)
                self.assertEqual(dir_member.mtime, 1600000000)

                bin_dir_member = members[f"{prefix}/bin"]
                self.assertTrue(bin_dir_member.isdir())
                self.assertEqual(bin_dir_member.mode, 0o755)

                script_member = members[f"{prefix}/bin/script.sh"]
                self.assertTrue(script_member.isfile())
                self.assertEqual(script_member.mode, 0o755)
                self.assertEqual(script_member.mtime, 1600000000)

                spec_member = members[f"{prefix}/packaging/fedora/kfaceauth.spec"]
                self.assertTrue(spec_member.isfile())
                self.assertEqual(spec_member.mode, 0o644)
                self.assertEqual(spec_member.mtime, 1600000000)

                prov_member = members[f"{prefix}/SOURCE-PROVENANCE.json"]
                self.assertTrue(prov_member.isfile())
                self.assertEqual(prov_member.mode, 0o644)
                self.assertEqual(prov_member.mtime, 1600000000)

                extracted_file = archive.extractfile(prov_member)
                self.assertIsNotNone(extracted_file)
                extracted_prov = json.loads(extracted_file.read().decode("utf-8"))
                self.assertEqual(extracted_prov["name"], "kfaceauth")
                self.assertEqual(extracted_prov["version"], "9.8.7")
                self.assertEqual(extracted_prov["source_date_epoch"], 1600000000)

    def test_create_archive_error_cleanup_and_main_cli(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)

            with patch.object(source, "source_files", side_effect=ValueError("forced error")):
                output = root / "out.tar.gz"
                with self.assertRaisesRegex(ValueError, "forced error"):
                    source.create_archive(root, output)
                self.assertFalse(output.exists())
                temp_files = list(root.glob("kfaceauth-source-*.tar.gz"))
                self.assertEqual(temp_files, [])

            output_cli = root / "cli-output.tar.gz"
            with patch.object(sys, "argv", ["create_source_archive.py", str(output_cli), "--root", str(root)]):
                exit_code = source.main()
                self.assertEqual(exit_code, 0)
                self.assertTrue(output_cli.is_file())

            with patch.object(sys, "argv", ["create_source_archive.py", "--root", str(root)]):
                exit_code = source.main()
                self.assertEqual(exit_code, 0)
                default_output = root / "kfaceauth-9.8.7.tar.gz"
                self.assertTrue(default_output.is_file())

            (root / "packaging/fedora/source-files.txt").write_text("invalid\n")
            with patch.object(sys, "argv", ["create_source_archive.py", "--root", str(root)]):
                with self.assertRaises(SystemExit) as cm:
                    source.main()
                self.assertEqual(cm.exception.code, 1)

    def test_tag_and_archive_lineage_rejected_before_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            source.create_archive(root, artifacts / "kfaceauth-9.8.7.tar.gz")
            with self.assertRaises(ValueError):
                release.validate_lineage(root, artifacts, "v1.0.0")
            commit = "a" * 40
            def command(*args):
                if "rev-parse" in args:
                    return commit
                return ""
            with patch.object(release, "run", side_effect=command):
                with self.assertRaisesRegex(ValueError, "lineage"):
                    release.validate_lineage(root, artifacts, "v9.8.7")

    def test_upload_identical_assets_is_idempotent_and_reads_back(self):
        self.check_upload("identical", expected_uploads=0)

    def test_upload_missing_assets_without_overwrite(self):
        self.check_upload("missing", expected_uploads=1)

    def test_upload_conflicting_assets_fails_without_mutation(self):
        self.check_upload("different", expected_uploads=0, reject=True)

    def check_upload(self, mode, expected_uploads, reject=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (artifacts / "safe.tar.gz").write_bytes(b"candidate")
            calls = []
            def command(*args):
                calls.append(args)
                if args[2] == "view":
                    return json.dumps({"assets": [] if mode == "missing" else [{"name": "safe.tar.gz"}]})
                if args[2] == "download":
                    destination = Path(args[args.index("--dir") + 1])
                    (destination / "safe.tar.gz").write_bytes(b"other" if mode == "different" else b"candidate")
                return ""
            with patch.object(release, "validate_lineage"), patch.object(release, "run", side_effect=command):
                if reject:
                    with self.assertRaisesRegex(ValueError, "refusing replacement"):
                        release.upload(root, artifacts, "v9.8.7", "owner/repo")
                else:
                    release.upload(root, artifacts, "v9.8.7", "owner/repo")
            uploads = [args for args in calls if args[2] == "upload"]
            self.assertEqual(len(uploads), expected_uploads)
            self.assertFalse(any("--clobber" in args for args in calls))
            if not reject:
                self.assertTrue(any("complete" in args[-1] for args in calls if args[2] == "download"))


if __name__ == "__main__":
    unittest.main()
