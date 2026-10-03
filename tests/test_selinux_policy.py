from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "data/selinux/kfaceauth_sddm.te"
FILE_CONTEXTS = ROOT / "data/selinux/kfaceauth_sddm.fc"
SETUP = ROOT / "data/pam/kfaceauth-pam-setup.sh"


class SddmSelinuxPolicyTests(unittest.TestCase):
    def test_sddm_access_is_limited_to_the_dedicated_runtime_type(self) -> None:
        policy = POLICY.read_text(encoding="utf-8")
        file_contexts = FILE_CONTEXTS.read_text(encoding="utf-8")

        self.assertIn("type kfaceauth_sddm_sock_t, file_type;", policy)
        self.assertIn(
            "allow xdm_t kfaceauth_sddm_sock_t:sock_file write;", policy
        )
        self.assertIn("allow xdm_t kfaceauth_sddm_sock_t:dir search;", policy)
        self.assertIn("allow xdm_t init_t:unix_stream_socket connectto;", policy)
        self.assertNotIn("allow xdm_t var_run_t:", policy)
        self.assertIn(
            "/run/kfaceauth(/.*)? system_u:object_r:kfaceauth_sddm_sock_t:s0",
            file_contexts,
        )

    def test_activation_installs_policy_before_changing_pam(self) -> None:
        setup = SETUP.read_text(encoding="utf-8")
        enable = setup.split("enable_pam() {", maxsplit=1)[1].split(
            "disable_pam() {", maxsplit=1
        )[0]
        data_cmake = (ROOT / "data/CMakeLists.txt").read_text(encoding="utf-8")
        spec = (ROOT / "packaging/fedora/kfaceauth.spec").read_text(
            encoding="utf-8"
        )

        self.assertLess(
            enable.index("install_selinux_policy"),
            enable.index('configure_target "$SDDM_PAM_FILE"'),
        )
        self.assertIn("semodule -i", setup)
        self.assertIn("semodule -r kfaceauth_sddm", setup)
        self.assertIn("restorecon -R -v /run/kfaceauth", setup)
        self.assertIn("selinux/kfaceauth_sddm.te", data_cmake)
        self.assertIn("selinux/kfaceauth_sddm.fc", data_cmake)
        self.assertIn("%{_datadir}/kfaceauth/selinux/kfaceauth_sddm.te", spec)
        self.assertIn("%{_datadir}/kfaceauth/selinux/kfaceauth_sddm.fc", spec)

    @unittest.skipUnless(
        shutil.which("checkmodule") and shutil.which("semodule_package"),
        "SELinux policy development tools are not installed",
    )
    def test_policy_compiles_and_packages_with_its_file_contexts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="kfaceauth-selinux-test-") as temp:
            module = Path(temp) / "kfaceauth_sddm.mod"
            package = Path(temp) / "kfaceauth_sddm.pp"
            subprocess.run(
                ["checkmodule", "-M", "-m", "-o", str(module), str(POLICY)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                [
                    "semodule_package",
                    "-o",
                    str(package),
                    "-m",
                    str(module),
                    "-f",
                    str(FILE_CONTEXTS),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertGreater(package.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
