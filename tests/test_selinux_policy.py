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
            r"/run/kfaceauth/kfaceauthd\.sock             gen_context(system_u:object_r:kfaceauth_sock_t,s0)",
            file_contexts,
        )
        self.assertNotIn("/run/kfaceauth(/.*)?", file_contexts)
        self.assertIn("allow xdm_t kfaceauth_sock_t:sock_file write;", sddm_policy)
        self.assertIn("allow xdm_t init_t:unix_stream_socket connectto;", sddm_policy)
        self.assertNotIn("allow xdm_t var_run_t:", sddm_policy)
        self.assertNotIn("mounton", main_policy + sddm_policy)
        self.assertNotIn("kfaceauth_sddm_sock_t", main_policy + sddm_policy + file_contexts)

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
        self.assertEqual(writable_path_sets, [["/var/lib/kfaceauth"]])

    def test_activation_has_target_specific_transactions_and_no_selinux_toggle(self) -> None:
        setup = SETUP.read_text(encoding="utf-8")
        enable = setup.split("enable_target() {", maxsplit=1)[1].split(
            "disable_target() {", maxsplit=1
        )[0]
        disable = setup.split("disable_target() {", maxsplit=1)[1].split(
            "[[ $# -ge 2 ]]", maxsplit=1
        )[0]
        cmake = (ROOT / "data/CMakeLists.txt").read_text(encoding="utf-8")
        spec = (ROOT / "packaging/fedora/kfaceauth.spec").read_text(encoding="utf-8")

        self.assertLess(enable.index("install_policy_modules"), enable.index("edit_pam_file"))
        self.assertIn("rollback_enable \"$file\" \"$backup\"", enable)
        self.assertIn("restore_pam_backup \"$file\" \"$backup\"", setup)
        self.assertIn("restore_pam_backup", disable)
        self.assertIn('rollback_enable "$file" "$backup"', enable)
        self.assertIn("restore_socket_state || failed=1", setup)
        self.assertIn("for ((index = ${#installed_modules[@]} - 1; index >= 0; index--)); do", setup)
        self.assertIn("for ((index = ${#removed_modules[@]} - 1; index >= 0; index--)); do", setup)
        self.assertIn("refusing to disable an unmanaged PAM rule", disable)
        self.assertIn("managed_rule_is_valid", setup)
        self.assertIn("is_managed \"$file\" || has_unmanaged_module_line \"$file\"", setup)
        self.assertIn("if ! is_managed \"$file\"", disable)
        self.assertIn("--enable-target sddm|plasma-lock", setup)
        self.assertIn("--disable-target sddm|plasma-lock", setup)
        self.assertIn("systemctl enable --now kfaceauth.socket", setup)
        self.assertIn("systemctl disable --now kfaceauth.socket", setup)
        self.assertIn("semodule -i", setup)
        self.assertIn("semodule -r", setup)
        self.assertIn("restorecon -R -v /run/kfaceauth", setup)
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
