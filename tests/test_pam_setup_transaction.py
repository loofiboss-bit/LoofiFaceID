from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "data/pam/kfaceauth-pam-setup.sh"

PAM_STACK = (
    "auth required pam_env.so\n"
    "auth substack password-auth\n"
    "account include password-auth\n"
)
MANAGED_BLOCK = (
    "# BEGIN kfaceauth experimental authentication\n"
    "auth        sufficient    pam_kfaceauth.so\n"
    "# END kfaceauth experimental authentication\n"
)

FAKE_COMMAND = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ["KFACEAUTH_FAKE_STATE_DIR"])
state_path = root / "systemctl.json"
modules_path = root / "modules.json"
labels_path = root / "labels.json"
log_path = root / "calls.jsonl"
fail_path = root / "fail-once"

def read_json(path, default):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else default

def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")

def log_call():
    with log_path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps({"command": name, "args": args}) + "\n")

def failure_mode():
    if not fail_path.exists():
        return None
    expected, mode = fail_path.read_text(encoding="utf-8").split("\n", 1)
    fixture_root = os.environ["FIXTURE_ROOT"]
    normalized = [arg.replace(fixture_root, "$ROOT") for arg in args]
    candidates = {
        name,
        name + " " + " ".join(normalized),
        name
        + " "
        + " ".join(
            Path(arg).name if arg.startswith(fixture_root) else arg for arg in args
        ),
    }
    if expected not in candidates:
        return None
    fail_path.unlink()
    return mode

log_call()
mode = failure_mode()
if mode == "before":
    raise SystemExit(1)

result = 0
if name == "getenforce":
    print(os.environ.get("KFACEAUTH_FAKE_SELINUX_MODE", "Enforcing"))
elif name == "systemctl":
    state = read_json(state_path, {})
    unit = args[-1] if args else ""
    key = "socket" if unit == "kfaceauth.socket" else "service"
    if args[:2] == ["is-enabled", "--quiet"]:
        result = 0 if state.get(key + "_enabled", False) else 1
    elif args[:2] == ["is-active", "--quiet"]:
        result = 0 if state.get(key + "_active", False) else 1
    elif args == ["daemon-reload"]:
        pass
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
    write_json(state_path, state)
elif name == "semodule":
    modules = read_json(modules_path, [])
    if args == ["-l"]:
        for module in modules:
            print(module + " 400")
    elif len(args) == 2 and args[0] == "-i":
        module = Path(args[1]).stem
        if module not in modules:
            modules.append(module)
        write_json(modules_path, modules)
    elif len(args) == 2 and args[0] == "-r":
        modules = [module for module in modules if module != args[1]]
        write_json(modules_path, modules)
    else:
        result = 2
elif name == "restorecon":
    labels = read_json(
        labels_path,
        {
            "runtime": "var_run_t",
            "socket": "var_run_t",
            "system_vault": "var_lib_t",
            "system_key": "etc_t",
        },
    )
    modules = read_json(modules_path, [])
    socket_type = "kfaceauth_sock_t" if "kfaceauth" in modules else "var_run_t"
    for arg in args:
        if arg.endswith("/run/kfaceauth"):
            labels["runtime"] = "var_run_t"
            labels["socket"] = socket_type
        elif arg.endswith("/run/kfaceauth/kfaceauthd.sock"):
            labels["socket"] = socket_type
        elif arg.endswith("/var/lib/kfaceauth"):
            labels["system_vault"] = (
                "kfaceauth_var_lib_t" if "kfaceauth" in modules else "var_lib_t"
            )
        elif arg.endswith("/etc/kfaceauth/keys"):
            labels["system_key"] = "kfaceauth_etc_t" if "kfaceauth" in modules else "etc_t"
    write_json(labels_path, labels)
elif name == "mv":
    paths = [arg for arg in args if arg != "-f" and arg != "--"]
    if len(paths) != 2:
        result = 2
    else:
        os.replace(paths[0], paths[1])
else:
    result = 2

if mode == "after":
    result = 1
raise SystemExit(result)
"""

HARNESS = r"""source "$KFACEAUTH_SETUP_SCRIPT"
KFACEAUTH_KDE_PAM_FILE="$FIXTURE_ROOT/etc/pam.d/kde"
KFACEAUTH_SDDM_PAM_FILE="$FIXTURE_ROOT/etc/pam.d/sddm"
KFACEAUTH_SELINUX_POLICY_DIR="$FIXTURE_ROOT/usr/share/kfaceauth/selinux"
KFACEAUTH_RUNTIME_DIRECTORY="$FIXTURE_ROOT/run/kfaceauth"
KFACEAUTH_SOCKET_PATH="$FIXTURE_ROOT/run/kfaceauth/kfaceauthd.sock"
KFACEAUTH_SYSTEM_VAULT_ROOT="$FIXTURE_ROOT/var/lib/kfaceauth"
KFACEAUTH_SYSTEM_KEY_ROOT="$FIXTURE_ROOT/etc/kfaceauth/keys"
KFACEAUTH_PAM_MODULE_PATHS=("$FIXTURE_ROOT/usr/lib64/security/pam_kfaceauth.so")
has_admin_rights() { return 0; }
if [[ $FIXTURE_ACTION == enable ]]; then
    enable_target "$FIXTURE_TARGET" 1000
else
    disable_target "$FIXTURE_TARGET"
fi
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
        (self.fixture_root / "usr/share/kfaceauth/selinux").mkdir(parents=True)
        (self.fixture_root / "usr/lib64/security").mkdir(parents=True)
        (self.fixture_root / "run/kfaceauth").mkdir(parents=True)
        self.bin_dir.mkdir()
        self.sddm = self.fixture_root / "etc/pam.d/sddm"
        self.kde = self.fixture_root / "etc/pam.d/kde"
        self.sddm.write_text(PAM_STACK, encoding="utf-8")
        self.kde.write_text(PAM_STACK, encoding="utf-8")
        (self.fixture_root / "var/lib/kfaceauth/1000/identity.vault").touch()
        (self.fixture_root / "etc/kfaceauth/keys/1000.key").touch()
        (self.fixture_root / "usr/lib64/security/pam_kfaceauth.so").touch()
        (self.fixture_root / "run/kfaceauth/kfaceauthd.sock").touch()
        for module in ("kfaceauth", "kfaceauth_sddm"):
            (self.fixture_root / f"usr/share/kfaceauth/selinux/{module}.pp").touch()
        for command in ("systemctl", "semodule", "restorecon", "getenforce", "mv"):
            path = self.bin_dir / command
            path.write_text(FAKE_COMMAND, encoding="utf-8")
            path.chmod(0o755)
        self.set_system_state(
            {"socket_enabled": False, "socket_active": False, "service_active": False}
        )
        self.set_modules([])
        self.set_labels(
            {
                "runtime": "var_run_t",
                "socket": "var_run_t",
                "system_vault": "var_lib_t",
                "system_key": "etc_t",
            }
        )

    @property
    def env(self) -> dict[str, str]:
        return {
            **os.environ,
            "PATH": f"{self.bin_dir}:{os.environ['PATH']}",
            "FIXTURE_ROOT": str(self.fixture_root),
            "FIXTURE_ACTION": "",
            "FIXTURE_TARGET": "sddm",
            "KFACEAUTH_SETUP_SCRIPT": str(SETUP),
            "KFACEAUTH_FAKE_STATE_DIR": str(self.state_dir),
        }

    def set_system_state(self, state: dict[str, bool]) -> None:
        (self.state_dir / "systemctl.json").write_text(
            json.dumps(state, sort_keys=True), encoding="utf-8"
        )

    def system_state(self) -> dict[str, bool]:
        return json.loads((self.state_dir / "systemctl.json").read_text(encoding="utf-8"))

    def set_modules(self, modules: list[str]) -> None:
        (self.state_dir / "modules.json").write_text(
            json.dumps(modules, sort_keys=True), encoding="utf-8"
        )

    def set_labels(self, labels: dict[str, str]) -> None:
        (self.state_dir / "labels.json").write_text(
            json.dumps(labels, sort_keys=True), encoding="utf-8"
        )

    def modules(self) -> list[str]:
        return json.loads((self.state_dir / "modules.json").read_text(encoding="utf-8"))

    def labels(self) -> dict[str, str]:
        return json.loads((self.state_dir / "labels.json").read_text(encoding="utf-8"))

    def run(
        self,
        action: str,
        target: str = "sddm",
        fail: str | None = None,
        fail_mode: str = "after",
    ) -> subprocess.CompletedProcess[str]:
        env = self.env
        env["FIXTURE_ACTION"] = action
        env["FIXTURE_TARGET"] = target
        if fail is not None:
            (self.state_dir / "fail-once").write_text(f"{fail}\n{fail_mode}", encoding="utf-8")
        return subprocess.run(
            ["bash", "-c", HARNESS],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )


class PamSetupTransactionTests(unittest.TestCase):
    def make_sandbox(self) -> tuple[tempfile.TemporaryDirectory[str], PamSetupSandbox]:
        temporary = tempfile.TemporaryDirectory(prefix="kfaceauth-setup-transaction-")
        return temporary, PamSetupSandbox(Path(temporary.name))

    def test_enable_and_repeat_are_idempotent(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            for _ in range(2):
                result = sandbox.run("enable")
                self.assertEqual(result.returncode, 0, result.stderr)
            contents = sandbox.sddm.read_text(encoding="utf-8")
            self.assertEqual(contents.count("# BEGIN kfaceauth experimental authentication"), 1)
            self.assertIn("auth substack password-auth\n", contents)
            self.assertEqual(sorted(sandbox.modules()), ["kfaceauth", "kfaceauth_sddm"])
            self.assertTrue(sandbox.system_state()["socket_enabled"])
            self.assertTrue(sandbox.system_state()["socket_active"])
            self.assertEqual(sandbox.labels()["socket"], "kfaceauth_sock_t")
            self.assertEqual(sandbox.labels()["system_vault"], "kfaceauth_var_lib_t")
            self.assertEqual(sandbox.labels()["system_key"], "kfaceauth_etc_t")

    def test_enable_adopts_the_exact_existing_rule(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            original = (
                "auth required pam_env.so\n"
                "auth        sufficient    pam_kfaceauth.so\n"
                "auth substack password-auth\n"
            )
            sandbox.sddm.write_text(original, encoding="utf-8")
            result = sandbox.run("enable")
            self.assertEqual(result.returncode, 0, result.stderr)
            contents = sandbox.sddm.read_text(encoding="utf-8")
            self.assertEqual(contents.count("pam_kfaceauth.so"), 1)
            self.assertIn(MANAGED_BLOCK, contents)
            self.assertIn("auth substack password-auth\n", contents)

    def test_disable_preserves_the_other_target_until_it_is_disabled(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            self.assertEqual(sandbox.run("enable", "sddm").returncode, 0)
            self.assertEqual(sandbox.run("enable", "plasma-lock").returncode, 0)

            disabled_sddm = sandbox.run("disable", "sddm")
            self.assertEqual(disabled_sddm.returncode, 0, disabled_sddm.stderr)
            self.assertNotIn(MANAGED_BLOCK, sandbox.sddm.read_text(encoding="utf-8"))
            self.assertIn(MANAGED_BLOCK, sandbox.kde.read_text(encoding="utf-8"))
            self.assertTrue(sandbox.system_state()["socket_enabled"])
            self.assertTrue(sandbox.system_state()["socket_active"])
            self.assertEqual(sandbox.modules(), ["kfaceauth"])
            self.assertEqual(sandbox.labels()["system_vault"], "kfaceauth_var_lib_t")
            self.assertEqual(sandbox.labels()["system_key"], "kfaceauth_etc_t")

            disabled_plasma = sandbox.run("disable", "plasma-lock")
            self.assertEqual(disabled_plasma.returncode, 0, disabled_plasma.stderr)
            self.assertFalse(sandbox.system_state()["socket_enabled"])
            self.assertFalse(sandbox.system_state()["socket_active"])
            self.assertEqual(sandbox.modules(), [])
            self.assertEqual(sandbox.labels()["system_vault"], "var_lib_t")
            self.assertEqual(sandbox.labels()["system_key"], "etc_t")

    def test_missing_password_fallback_leaves_all_state_unchanged(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            original = "auth required pam_env.so\n"
            sandbox.sddm.write_text(original, encoding="utf-8")
            before_state = sandbox.system_state()
            result = sandbox.run("enable")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sandbox.sddm.read_text(encoding="utf-8"), original)
            self.assertEqual(sandbox.system_state(), before_state)
            self.assertEqual(sandbox.modules(), [])
            self.assertEqual(
                sandbox.labels(),
                {
                    "runtime": "var_run_t",
                    "socket": "var_run_t",
                    "system_vault": "var_lib_t",
                    "system_key": "etc_t",
                },
            )

    def test_malformed_markers_leave_all_state_unchanged(self) -> None:
        temporary, sandbox = self.make_sandbox()
        with temporary:
            original = "# BEGIN kfaceauth experimental authentication\nauth required pam_unix.so\n"
            sandbox.sddm.write_text(original, encoding="utf-8")
            result = sandbox.run("enable")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sandbox.sddm.read_text(encoding="utf-8"), original)
            self.assertEqual(sandbox.modules(), [])
            self.assertFalse(sandbox.system_state()["socket_enabled"])
            self.assertEqual(
                sandbox.labels(),
                {
                    "runtime": "var_run_t",
                    "socket": "var_run_t",
                    "system_vault": "var_lib_t",
                    "system_key": "etc_t",
                },
            )

    def test_enable_failures_after_mutations_restore_prior_state(self) -> None:
        cases = (
            "semodule -i kfaceauth.pp",
            "semodule -i kfaceauth_sddm.pp",
            "restorecon -R -v $ROOT/run/kfaceauth",
            "restorecon -R -v $ROOT/var/lib/kfaceauth",
            "restorecon -R -v $ROOT/etc/kfaceauth/keys",
            "mv",
            "systemctl daemon-reload",
            "systemctl enable --now kfaceauth.socket",
            "restorecon -v $ROOT/run/kfaceauth/kfaceauthd.sock",
        )
        for failure in cases:
            with self.subTest(failure=failure):
                temporary, sandbox = self.make_sandbox()
                with temporary:
                    sandbox.set_system_state(
                        {
                            "socket_enabled": True,
                            "socket_active": True,
                            "service_active": True,
                        }
                    )
                    before_pam = sandbox.sddm.read_text(encoding="utf-8")
                    before_state = sandbox.system_state()
                    before_labels = sandbox.labels()
                    result = sandbox.run("enable", fail=failure)
                    self.assertNotEqual(result.returncode, 0, failure)
                    self.assertEqual(sandbox.sddm.read_text(encoding="utf-8"), before_pam)
                    self.assertEqual(sandbox.system_state(), before_state)
                    self.assertEqual(sandbox.modules(), [])
                    self.assertEqual(sandbox.labels(), before_labels)

    def test_disable_failures_after_mutations_restore_prior_state(self) -> None:
        cases = (
            "mv",
            "systemctl disable --now kfaceauth.socket",
            "semodule -r kfaceauth_sddm",
            "semodule -r kfaceauth",
            "restorecon -R -v $ROOT/run/kfaceauth",
            "restorecon -R -v $ROOT/var/lib/kfaceauth",
            "restorecon -R -v $ROOT/etc/kfaceauth/keys",
        )
        for failure in cases:
            with self.subTest(failure=failure):
                temporary, sandbox = self.make_sandbox()
                with temporary:
                    enabled = sandbox.run("enable")
                    self.assertEqual(enabled.returncode, 0, enabled.stderr)
                    sandbox.set_system_state(
                        {
                            "socket_enabled": True,
                            "socket_active": True,
                            "service_active": True,
                        }
                    )
                    before_pam = sandbox.sddm.read_text(encoding="utf-8")
                    before_state = sandbox.system_state()
                    before_modules = sorted(sandbox.modules())
                    before_labels = sandbox.labels()
                    result = sandbox.run("disable", fail=failure)
                    self.assertNotEqual(result.returncode, 0, failure)
                    self.assertEqual(sandbox.sddm.read_text(encoding="utf-8"), before_pam)
                    self.assertEqual(sandbox.system_state(), before_state)
                    self.assertEqual(sorted(sandbox.modules()), before_modules)
                    self.assertEqual(sandbox.labels(), before_labels)


if __name__ == "__main__":
    unittest.main()
