from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "data/pam/kfaceauth-pam-setup.sh"

FAKE_COMMAND = r"""#!/usr/bin/env python3
import json
import os
import stat
import sys
from pathlib import Path

name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ["KFACEAUTH_FAKE_STATE_DIR"])
state_path = root / "state.json"
modules_path = root / "modules.json"
calls_path = root / "calls.jsonl"
fail_path = root / "fail-once"

def read_json(path, default):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else default

with calls_path.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps({"command": name, "args": args}) + "\n")

failure = None
if fail_path.exists():
    expected, failure = fail_path.read_text(encoding="utf-8").split("\n", 1)
    if expected != name + " " + " ".join(args):
        failure = None

result = 0
if name == "getenforce":
    print(os.environ.get("KFACEAUTH_FAKE_SELINUX_MODE", "Enforcing"))
elif name == "semodule":
    modules = read_json(modules_path, [])
    if args == ["-l"]:
        for module in modules:
            print(module + " 400")
    elif len(args) == 2 and args[0] == "-i":
        module = Path(args[1]).stem
        if module not in modules:
            modules.append(module)
        modules_path.write_text(json.dumps(modules), encoding="utf-8")
    elif len(args) == 2 and args[0] == "-r":
        modules = [module for module in modules if module != args[1]]
        modules_path.write_text(json.dumps(modules), encoding="utf-8")
    else:
        result = 2
elif name == "restorecon":
    pass
elif name == "chown":
    pass
elif name == "stat":
    metadata = os.stat(args[-1], follow_symlinks=False)
    if len(args) > 1 and args[1] == "%u":
        print("0")
    else:
        print(f"0:0:{stat.S_IMODE(metadata.st_mode):o}")
elif name == "systemctl":
    state = read_json(state_path, {})
    unit = args[-1] if args else ""
    key = "socket" if unit == "kfaceauth.socket" else "service"
    if args[:2] == ["is-enabled", "--quiet"]:
        result = 0 if state.get(key + "_enabled", False) else 1
    elif args[:2] == ["is-active", "--quiet"]:
        result = 0 if state.get(key + "_active", False) else 1
    elif args[:1] == ["enable"]:
        state[key + "_enabled"] = True
        if "--now" in args:
            state[key + "_active"] = True
    elif args[:1] == ["disable"]:
        state[key + "_enabled"] = False
        if "--now" in args:
            state[key + "_active"] = False
    elif args[:1] == ["start"]:
        state[key + "_active"] = True
    elif args[:1] == ["stop"]:
        state[key + "_active"] = False
    else:
        result = 2
    state_path.write_text(json.dumps(state, sort_keys=True), encoding="utf-8")
else:
    result = 2

if failure is not None:
    fail_path.unlink(missing_ok=True)
    if failure == "after":
        result = 1
    elif failure == "before":
        raise SystemExit(1)
raise SystemExit(result)
"""

HARNESS = r"""source "$KFACEAUTH_SETUP_SCRIPT"
KFACEAUTH_PAM_DIRECTORY="$FIXTURE_ROOT/etc/pam.d"
KFACEAUTH_PAM_TEMPLATE_DIRECTORY="$FIXTURE_ROOT/usr/share/kfaceauth/pam"
KFACEAUTH_SELINUX_POLICY_DIR="$FIXTURE_ROOT/usr/share/kfaceauth/selinux"
KFACEAUTH_RUNTIME_DIRECTORY="$FIXTURE_ROOT/run/kfaceauth"
KFACEAUTH_SOCKET_PATH="$FIXTURE_ROOT/run/kfaceauth/kfaceauthd.sock"
KFACEAUTH_SYSTEM_VAULT_ROOT="$FIXTURE_ROOT/var/lib/kfaceauth"
KFACEAUTH_SYSTEM_KEY_ROOT="$FIXTURE_ROOT/etc/kfaceauth/keys"
KFACEAUTH_PAM_MODULE_PATHS=("$FIXTURE_ROOT/usr/lib64/security/pam_kfaceauth.so")
has_admin_rights() { return 0; }
prepare_target "$FIXTURE_TARGET" 1000
"""


class PamSetupSandbox:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.state_dir = root / "state"
        self.fixture_root = root / "system"
        self.bin_dir = root / "bin"
        self.state_dir.mkdir()
        (self.fixture_root / "etc/pam.d").mkdir(parents=True)
        (self.fixture_root / "etc/kfaceauth/keys").mkdir(parents=True)
        (self.fixture_root / "var/lib/kfaceauth/1000").mkdir(parents=True)
        (self.fixture_root / "usr/share/kfaceauth/pam").mkdir(parents=True)
        (self.fixture_root / "usr/share/kfaceauth/selinux").mkdir(parents=True)
        (self.fixture_root / "usr/lib64/security").mkdir(parents=True)
        (self.fixture_root / "run/kfaceauth").mkdir(parents=True)
        self.bin_dir.mkdir()
        self.sddm = self.fixture_root / "etc/pam.d/sddm"
        self.kde = self.fixture_root / "etc/pam.d/kde"
        self.sddm_original = (
            "auth required pam_env.so\n"
            "auth substack password-auth\n"
            "account include password-auth\n"
        )
        self.kde_original = (
            "auth required pam_env.so\n"
            "auth substack system-auth\n"
            "account include system-auth\n"
        )
        self.sddm.write_text(self.sddm_original, encoding="utf-8")
        self.kde.write_text(self.kde_original, encoding="utf-8")
        self.write_template("sddm-kfaceauth")
        self.write_template("kde-kfaceauth")
        (self.fixture_root / "var/lib/kfaceauth/1000/identity.vault").write_bytes(b"vault")
        (self.fixture_root / "etc/kfaceauth/keys/1000.key").write_bytes(b"key")
        (self.fixture_root / "usr/lib64/security/pam_kfaceauth.so").touch()
        (self.fixture_root / "run/kfaceauth/kfaceauthd.sock").touch()
        for module in ("kfaceauth", "kfaceauth_sddm"):
            (self.fixture_root / f"usr/share/kfaceauth/selinux/{module}.pp").touch()
        for command in ("systemctl", "semodule", "restorecon", "getenforce", "chown", "stat"):
            path = self.bin_dir / command
            path.write_text(FAKE_COMMAND, encoding="utf-8")
            path.chmod(0o755)
        self.set_modules([])
        self.set_state({"socket_enabled": False, "socket_active": False, "service_active": False})

    def write_template(self, service: str) -> None:
        target = "sddm" if service == "sddm-kfaceauth" else "kde"
        text = (
            "# Dedicated KFaceAuth PAM test service\n"
            "auth sufficient pam_kfaceauth.so\n"
            f"auth include {target}\n"
            f"account include {target}\n"
            f"password include {target}\n"
            f"session include {target}\n"
        )
        (self.fixture_root / f"usr/share/kfaceauth/pam/{service}").write_text(
            text, encoding="utf-8"
        )

    @property
    def env(self) -> dict[str, str]:
        return {
            **os.environ,
            "PATH": f"{self.bin_dir}:{os.environ['PATH']}",
            "FIXTURE_ROOT": str(self.fixture_root),
            "FIXTURE_TARGET": "sddm",
            "KFACEAUTH_SETUP_SCRIPT": str(SETUP),
            "KFACEAUTH_FAKE_STATE_DIR": str(self.state_dir),
        }

    def state(self) -> dict[str, bool]:
        return json.loads((self.state_dir / "state.json").read_text(encoding="utf-8"))

    def set_state(self, value: dict[str, bool]) -> None:
        (self.state_dir / "state.json").write_text(json.dumps(value), encoding="utf-8")

    def modules(self) -> list[str]:
        return json.loads((self.state_dir / "modules.json").read_text(encoding="utf-8"))

    def set_modules(self, value: list[str]) -> None:
        (self.state_dir / "modules.json").write_text(json.dumps(value), encoding="utf-8")

    def run(self, target: str = "sddm", fail: str | None = None) -> subprocess.CompletedProcess[str]:
        env = self.env
        env["FIXTURE_TARGET"] = target
        if fail is not None:
            (self.state_dir / "fail-once").write_text(f"{fail}\nafter", encoding="utf-8")
        return subprocess.run(
            ["bash", "-c", HARNESS], env=env, capture_output=True, text=True, check=False
        )


class PamSetupTransactionTests(unittest.TestCase):
    def make_sandbox(self) -> tuple[tempfile.TemporaryDirectory[str], PamSetupSandbox]:
        temporary = tempfile.TemporaryDirectory(prefix="kfaceauth-setup-transaction-")
        return temporary, PamSetupSandbox(Path(temporary.name))

    def assert_standard_services_unchanged(self, sandbox: PamSetupSandbox) -> None:
        self.assertEqual(sandbox.sddm.read_text(encoding="utf-8"), sandbox.sddm_original)
        self.assertEqual(sandbox.kde.read_text(encoding="utf-8"), sandbox.kde_original)

    def test_prepare_adds_only_the_dedicated_service_and_is_idempotent(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            prepared = sandbox.run("sddm")
            self.assertEqual(prepared.returncode, 0, prepared.stderr)
            dedicated = sandbox.fixture_root / "etc/pam.d/sddm-kfaceauth"
            template = sandbox.fixture_root / "usr/share/kfaceauth/pam/sddm-kfaceauth"
            self.assertEqual(dedicated.read_bytes(), template.read_bytes())
            self.assert_standard_services_unchanged(sandbox)
            self.assertEqual(sandbox.modules(), ["kfaceauth", "kfaceauth_sddm"])
            self.assertTrue(sandbox.state()["socket_enabled"])
            self.assertTrue(sandbox.state()["socket_active"])

            repeated = sandbox.run("sddm")
            self.assertEqual(repeated.returncode, 0, repeated.stderr)
            self.assertEqual(dedicated.read_bytes(), template.read_bytes())
            self.assert_standard_services_unchanged(sandbox)

    def test_prepare_removes_only_exact_legacy_global_face_blocks_transactionally(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            begin = "# BEGIN kfaceauth experimental authentication\n"
            face_rule = "auth        sufficient    pam_kfaceauth.so\n"
            end = "# END kfaceauth experimental authentication\n"
            sandbox.sddm_original = (
                "auth required pam_env.so\n" + begin + face_rule + end + "auth substack password-auth\n"
                "account include password-auth\n"
            )
            sandbox.kde_original = (
                "auth required pam_env.so\n" + begin + face_rule + end + "auth substack system-auth\n"
                "account include system-auth\n"
            )
            sandbox.sddm.write_text(sandbox.sddm_original, encoding="utf-8")
            sandbox.kde.write_text(sandbox.kde_original, encoding="utf-8")

            prepared = sandbox.run("sddm")

            self.assertEqual(prepared.returncode, 0, prepared.stderr)
            self.assertEqual(
                sandbox.sddm.read_text(encoding="utf-8"),
                "auth required pam_env.so\nauth substack password-auth\naccount include password-auth\n",
            )
            self.assertEqual(
                sandbox.kde.read_text(encoding="utf-8"),
                "auth required pam_env.so\nauth substack system-auth\naccount include system-auth\n",
            )
            self.assertFalse(any(sandbox.fixture_root.glob("etc/pam.d/*.kfaceauth-legacy-backup.*")))

    def test_malformed_legacy_block_rolls_back_preparation_and_preserves_both_pam_files(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            original_sddm = (
                "auth required pam_env.so\n"
                "# BEGIN kfaceauth experimental authentication\n"
                "auth        sufficient    pam_kfaceauth.so\n"
                "# END kfaceauth experimental authentication\n"
                "auth substack password-auth\n"
            )
            original_kde = (
                "auth required pam_env.so\n"
                "# BEGIN kfaceauth experimental authentication\n"
                "auth        sufficient    pam_kfaceauth.so\n"
                "auth substack system-auth\n"
            )
            sandbox.sddm.write_text(original_sddm, encoding="utf-8")
            sandbox.kde.write_text(original_kde, encoding="utf-8")

            result = sandbox.run("sddm")

            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sandbox.sddm.read_text(encoding="utf-8"), original_sddm)
            self.assertEqual(sandbox.kde.read_text(encoding="utf-8"), original_kde)
            self.assertEqual(sandbox.modules(), [])
            self.assertEqual(
                sandbox.state(),
                {"socket_enabled": False, "socket_active": False, "service_active": False},
            )
            self.assertFalse((sandbox.fixture_root / "etc/pam.d/sddm-kfaceauth").exists())

    def test_plasma_target_prepares_its_service_without_sddm_policy(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            prepared = sandbox.run("plasma-lock")
            self.assertEqual(prepared.returncode, 0, prepared.stderr)
            dedicated = sandbox.fixture_root / "etc/pam.d/kde-kfaceauth"
            template = sandbox.fixture_root / "usr/share/kfaceauth/pam/kde-kfaceauth"
            self.assertEqual(dedicated.read_bytes(), template.read_bytes())
            self.assertEqual(sandbox.modules(), ["kfaceauth"])
            self.assert_standard_services_unchanged(sandbox)

    def test_custom_dedicated_file_is_preserved_and_rejected(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            dedicated = sandbox.fixture_root / "etc/pam.d/sddm-kfaceauth"
            dedicated.write_text("# administrator-owned\n", encoding="utf-8")
            result = sandbox.run("sddm")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(dedicated.read_text(encoding="utf-8"), "# administrator-owned\n")
            self.assertEqual(sandbox.modules(), [])
            self.assert_standard_services_unchanged(sandbox)

    def test_identical_but_group_writable_dedicated_file_is_rejected(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            dedicated = sandbox.fixture_root / "etc/pam.d/sddm-kfaceauth"
            template = sandbox.fixture_root / "usr/share/kfaceauth/pam/sddm-kfaceauth"
            dedicated.write_bytes(template.read_bytes())
            dedicated.chmod(0o664)

            result = sandbox.run("sddm")

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsafe ownership or permissions", result.stderr)
            self.assertEqual(dedicated.stat().st_mode & 0o777, 0o664)
            self.assertEqual(sandbox.modules(), [])
            self.assert_standard_services_unchanged(sandbox)

    def test_socket_failure_rolls_back_new_service_and_policy_module(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            result = sandbox.run("sddm", fail="systemctl enable --now kfaceauth.socket")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((sandbox.fixture_root / "etc/pam.d/sddm-kfaceauth").exists())
            self.assertEqual(sandbox.modules(), [])
            self.assertEqual(
                sandbox.state(),
                {"socket_enabled": False, "socket_active": False, "service_active": False},
            )
            self.assert_standard_services_unchanged(sandbox)

    def test_missing_template_does_not_mutate_any_authentication_state(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            (sandbox.fixture_root / "usr/share/kfaceauth/pam/sddm-kfaceauth").unlink()
            result = sandbox.run("sddm")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((sandbox.fixture_root / "etc/pam.d/sddm-kfaceauth").exists())
            self.assertEqual(sandbox.modules(), [])
            self.assert_standard_services_unchanged(sandbox)

    def test_cli_is_versioned_and_rejects_old_global_mutation_commands(self) -> None:
        version = subprocess.run(["bash", str(SETUP), "--version"], capture_output=True, text=True)
        self.assertEqual(version.returncode, 0)
        self.assertEqual(version.stdout.strip(), "kfaceauth-pam-setup 2")

        old = subprocess.run(
            ["bash", str(SETUP), "--enable-target", "sddm", "--uid", "1000"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(old.returncode, 0)
        self.assertIn("--prepare-target", old.stderr)


if __name__ == "__main__":
    unittest.main()
