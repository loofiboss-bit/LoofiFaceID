from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_POLICY = ROOT / "data/selinux/kfaceauth.te"
SDDM_POLICY = ROOT / "data/selinux/kfaceauth_sddm.te"
FILE_CONTEXTS = ROOT / "data/selinux/kfaceauth.fc"
SETUP = ROOT / "data/pam/kfaceauth-pam-setup.sh"
SERVICE = ROOT / "data/systemd/kfaceauth.service"
SOCKET = ROOT / "data/systemd/kfaceauth.socket"


class ExperimentalAuthBoundaryTests(unittest.TestCase):
    def test_pam_setup_script_has_valid_bash_syntax(self) -> None:
        subprocess.run(["bash", "-n", str(SETUP)], check=True, capture_output=True, text=True)

    def test_runtime_label_is_socket_only_and_sddm_access_is_narrow(self) -> None:
        main_policy = MAIN_POLICY.read_text(encoding="utf-8")
        sddm_policy = SDDM_POLICY.read_text(encoding="utf-8")
        file_contexts = FILE_CONTEXTS.read_text(encoding="utf-8")

        self.assertIn(
            r"/run/kfaceauth/kfaceauthd\.sock             system_u:object_r:kfaceauth_sock_t:s0",
            file_contexts,
        )
        self.assertNotIn("gen_context", file_contexts)
        self.assertNotIn("/run/kfaceauth(/.*)?", file_contexts)
        self.assertIn("allow xdm_t kfaceauth_sock_t:sock_file write;", sddm_policy)
        self.assertIn("allow xdm_t kfaceauth_t:unix_stream_socket connectto;", sddm_policy)
        self.assertNotIn("allow xdm_t init_t:unix_stream_socket connectto;", sddm_policy)
        self.assertNotIn("allow xdm_t domain:unix_stream_socket connectto;", sddm_policy)
        self.assertNotIn("allow xdm_t var_run_t:", sddm_policy)
        self.assertIn("allow init_t kfaceauth_var_lib_t:dir mounton;", main_policy)
        self.assertNotIn("allow init_t var_lib_t:dir mounton;", main_policy)
        self.assertNotIn("mounton", sddm_policy)
        self.assertIn("role system_r types kfaceauth_t;", main_policy)
        self.assertIn(
            "allow kfaceauth_t kfaceauth_exec_t:file { entrypoint ioctl lock map execute getattr open read };",
            main_policy,
        )
        self.assertIn("allow kfaceauth_t kfaceauth_exec_t:file execute_no_trans;", main_policy)
        self.assertIn("allow init_t kfaceauth_t:process2 nnp_transition;", main_policy)
        self.assertIn(
            "allow init_t kfaceauth_t:unix_stream_socket { create bind listen getattr setopt getopt };",
            main_policy,
        )
        self.assertNotIn("allow init_t domain:process2", main_policy)
        self.assertNotIn("kfaceauth_sddm_sock_t", main_policy + sddm_policy + file_contexts)
        self.assertNotIn("sock_file_type", main_policy)
        self.assertIn("type kfaceauth_sock_t, file_type;", main_policy)
        self.assertIn("type v4l_device_t;", main_policy)
        self.assertIn("allow kfaceauth_t v4l_device_t:chr_file { read write open getattr ioctl map };", main_policy)
        self.assertNotIn("video_device_t", main_policy)
        self.assertIn("/usr/libexec/kfaceauth-auth-worker", file_contexts)
        self.assertIn("allow kfaceauth_t self:process { fork sigkill sigchld };", main_policy)
        self.assertIn("allow kfaceauth_t self:process setrlimit;", main_policy)
        self.assertNotIn("allow kfaceauth_t domain:process setrlimit;", main_policy)
        self.assertNotIn("allow kfaceauth_t domain:process", main_policy)

    def test_socket_activated_daemon_does_not_mount_runtime_directory_writable(self) -> None:
        service = SERVICE.read_text(encoding="utf-8")
        socket = SOCKET.read_text(encoding="utf-8")
        writable_path_sets = [
            line.partition("=")[2].split()
            for line in service.splitlines()
            if line.startswith("ReadWritePaths=")
        ]

        self.assertIn("Requires=kfaceauth.socket", service)
        self.assertIn("ListenStream=/run/kfaceauth/kfaceauthd.sock", socket)
        self.assertEqual(writable_path_sets, [])
        read_only_path_sets = [
            line.partition("=")[2].split()
            for line in service.splitlines()
            if line.startswith("ReadOnlyPaths=")
        ]
        self.assertIn("/var/lib/kfaceauth", read_only_path_sets[0])
        main_policy = MAIN_POLICY.read_text(encoding="utf-8")
        vault_permissions = next(
            line for line in main_policy.splitlines()
            if line.startswith("allow kfaceauth_t kfaceauth_var_lib_t:file")
        )
        key_permissions = next(
            line for line in main_policy.splitlines()
            if line.startswith("allow kfaceauth_t kfaceauth_etc_t:file")
        )
        for permissions in (vault_permissions, key_permissions):
            self.assertIn("read", permissions)
            self.assertNotRegex(permissions, r"\b(write|create|unlink|lock)\b")

    def test_preparation_uses_dedicated_service_and_transactional_migration(self) -> None:
        setup = SETUP.read_text(encoding="utf-8")
        cmake = (ROOT / "data/CMakeLists.txt").read_text(encoding="utf-8")
        spec = (ROOT / "packaging/fedora/kfaceauth.spec").read_text(encoding="utf-8")

        self.assertIn("--prepare-target sddm|plasma-lock", setup)
        self.assertIn("sddm-kfaceauth", setup)
        self.assertIn("kde-kfaceauth", setup)
        self.assertIn("install_dedicated_pam_service", setup)
        self.assertIn("migrate_legacy_global_rules", setup)
        self.assertIn("restore_legacy_pam_backups", setup)
        self.assertIn("KFACEAUTH_LEGACY_BEGIN", setup)
        self.assertIn("KFACEAUTH_LEGACY_END", setup)
        self.assertIn("found an unmanaged global face-authentication rule; preserving", setup)
        self.assertIn("standard PAM services were left unchanged", setup)
        self.assertIn("restore_socket_state || failed=1", setup)
        self.assertIn("for ((index = ${#installed_modules[@]} - 1; index >= 0; index--)); do", setup)
        self.assertIn("systemctl enable --now kfaceauth.socket", setup)
        self.assertIn("semodule -i", setup)
        self.assertIn("semodule -r", setup)
        self.assertIn('restorecon -R -v "$path"', setup)
        self.assertNotIn("setenforce", setup)
        self.assertIn("set(KFACEAUTH_SELINUX_MODULES kfaceauth kfaceauth_sddm)", cmake)
        self.assertIn("kfaceauth_sddm.pp", spec)
        self.assertNotIn("kfaceauth_sddm.fc", cmake + spec)

    @unittest.skipUnless(
        shutil.which("checkmodule") and shutil.which("semodule_package"),
        "SELinux policy development tools are not installed",
    )
    def test_policy_modules_compile_and_package(self) -> None:
        with tempfile.TemporaryDirectory(prefix="kfaceauth-selinux-test-") as temp:
            temp_root = Path(temp)
            for name, source, file_contexts in (
                ("kfaceauth", MAIN_POLICY, FILE_CONTEXTS),
                ("kfaceauth_sddm", SDDM_POLICY, None),
            ):
                module = temp_root / f"{name}.mod"
                package = temp_root / f"{name}.pp"
                subprocess.run(
                    ["checkmodule", "-M", "-m", "-o", str(module), str(source)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                command = ["semodule_package", "-o", str(package), "-m", str(module)]
                if file_contexts is not None:
                    command.extend(["-f", str(file_contexts)])
                subprocess.run(command, check=True, capture_output=True, text=True)
                self.assertGreater(package.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
