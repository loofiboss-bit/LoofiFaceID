# Milestone 4 Qualification: System Daemon, PAM Integration & Security Architecture

> **Status:** Historical roadmap material. PAM, SDDM, daemon, and system
> authentication are outside the current beginner flow and remain unqualified
> until independently reproduced on the target Fedora 44 system.

This document records historical code paths, security design notes, and test
procedures for Milestone 4 (v5.0). The statuses below are archival engineering
claims, not current release evidence; they do not authorize PAM, daemon, SDDM,
or system-authentication use in the beginner flow.

---

## 1. Implemented Subsystems & Architecture

| Component | Target File(s) | Implementation Evidence | Security / Operational Guarantee |
|---|---|---|---|
| **System Vault** | `engine/templates/src/lib.rs` | `VaultKind::System`, `Vault::system(uid)`, `migrate_legacy_vault` | Path: `/var/lib/kfaceauth/<uid>/identity.vault`<br>Directory DAC: `0750` (`mode & 0o007 == 0`, `mode & 0o020 == 0`)<br>File DAC: `0640` (`mode & 0o007 == 0`, `mode & 0o020 == 0`)<br>Ownership: `<uid>:kfaceauth`<br>No `CAP_DAC_OVERRIDE` required |
| **Migration CLI** | `engine/templates/src/bin/kfaceauth-migrate-vault.rs` | CLI tool with target UID selection and zeroized sensitive buffers | Copies legacy user-session vault to system directory and validates format before atomic swap |
| **Native FFI & Credentials** | `engine/crypto-openssl-sys/native/crypto_bridge.c`, `.h`<br>`engine/crypto-openssl-sys/src/lib.rs` | `peer_credentials(raw_fd)`, `drop_privileges()`, `master_key_for_uid()`, `systemd_socket_listener()` | Uses `SO_PEERCRED` to extract kernel-verified `uid`/`gid`/`pid`. Zeroizes master key buffers immediately. |
| **Standalone Daemon** | `engine/daemon/src/lib.rs`<br>`engine/daemon/src/main.rs` | Workspace crate `kfaceauth-daemon`, binary `kfaceauthd` (`#![forbid(unsafe_code)]`) | Socket activation via `/run/kfaceauth/kfaceauthd.sock`. Drops root to `kfaceauth:kfaceauth`. Authorizes caller UID against target UID. |
| **Thin PAM Module** | `pam/src/pam_kfaceauth.c`<br>`pam/CMakeLists.txt` | `pam_kfaceauth.so` C17 shared library | Socket timeouts set to $\le 2.0\text{s}$ via `SO_RCVTIMEO`/`SO_SNDTIMEO`. Fails closed to `PAM_AUTH_ERR` without user prompts or GUI dialogs. |
| **Systemd Units** | `data/systemd/kfaceauth.socket`<br>`data/systemd/kfaceauth.service` | Mode `0660`, group `kfaceauth`. Service runs as `User=kfaceauth`, `ProtectSystem=strict`, `NoNewPrivileges=true`, `CapabilityBoundingSet=` (empty) | Socket-activated on demand; zero ambient Linux capabilities. |
| **SELinux Confinement** | `data/selinux/kfaceauth.te`, `.fc`, `.if` | Defines domain `kfaceauth_t` and file contexts for socket, data, and config | Confinement validated with `checkmodule` / `semodule_package`. |

---

## 2. Acceptance Gate Qualifications

### Gate 4.1: Root Privilege Drop & Elimination of `CAP_DAC_OVERRIDE`
- **Criterion**: Daemon drops root privileges immediately upon socket creation; worker executes strictly as `kfaceauth:kfaceauth` without requiring `CAP_DAC_OVERRIDE`.
- **Verification**:
  - `data/systemd/kfaceauth.service` explicitly specifies:
    ```ini
    User=kfaceauth
    Group=kfaceauth
    CapabilityBoundingSet=
    AmbientCapabilities=
    ProtectSystem=strict
    NoNewPrivileges=true
    ```
  - `kfaceauthd` invokes `kfaceauth_drop_privileges("kfaceauth", "kfaceauth")` on startup.
  - Automated test `tests/test_m4_gates.py:test_gate_4_1_daemon_drops_root_and_no_cap_dac_override` passes.
- **Status**: **HISTORICAL / NOT CURRENTLY QUALIFIED**

### Gate 4.2: PAM Module Abort Within $\le 2.0\text{s}$
- **Criterion**: PAM module aborts within $\le 2.0\text{ seconds}$ if camera is busy or user is absent, falling back to password prompt without error dialogs.
- **Verification**:
  - `pam/src/pam_kfaceauth.c` configures `struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };` for both `SO_RCVTIMEO` and `SO_SNDTIMEO`.
  - CTest `tests/unit/test_pam.cpp` exercises hung mock server: verified that `pam_sm_authenticate` returns `PAM_AUTH_ERR` within $2.01\text{s}$.
  - Camera-busy simulation (`STATUS_DEVICE_BUSY`) immediately returns `PAM_AUTH_ERR` in $<10\text{ms}$.
  - Automated test `tests/test_m4_gates.py:test_gate_4_2_pam_timeout_abort_under_two_seconds` passes.
- **Status**: **HISTORICAL / NOT CURRENTLY QUALIFIED**

### Gate 4.3: Cross-UID Access & Tamper Defense
- **Criterion**: Process running as UID 1001 cannot query, decrypt, or tamper with UID 1000's vault.
- **Verification**:
  - `kfaceauthd` enforces `peer_uid == 0 || peer_uid == target_uid` in `authorize_peer()`.
  - Automated unit test `tests::gate_4_3_cross_uid_tamper_defense_all_opcodes` tests an attacker process (UID 1001) attempting each protocol opcode against UID 1000:
    - `OP_PAM_AUTH` $\rightarrow$ `STATUS_ACCESS_DENIED`
    - `OP_STATUS` $\rightarrow$ `STATUS_ACCESS_DENIED`
    - `OP_GET_KEY` $\rightarrow$ `STATUS_ACCESS_DENIED`
    - `OP_VERIFY_FRAME` $\rightarrow$ `STATUS_ACCESS_DENIED`
    - `OP_DELETE_PROFILE` $\rightarrow$ `STATUS_ACCESS_DENIED`
  - Automated test `tests/test_m4_gates.py:test_gate_4_3_cross_uid_access_rejection` passes.
- **Status**: **HISTORICAL / NOT CURRENTLY QUALIFIED**

---

## 3. Reproducible Verification Commands

To verify all components locally:

```bash
# 1. Run full Python test suite (including M4 gates, packaging, and security boundary):
python3 -m unittest discover -s tests -p 'test_*.py'

# 2. Run CTest suite (including full PAM conversation tests):
ctest --test-dir build --output-on-failure

# 3. Run Rust Engine workspace tests:
cargo test --workspace --manifest-path engine/Cargo.toml

# 4. Verify formatting and linting:
cargo clippy --workspace --all-targets --manifest-path engine/Cargo.toml -- -D warnings
cargo fmt --manifest-path engine/Cargo.toml --all -- --check
clang-format --dry-run --Werror pam/src/pam_kfaceauth.c tests/unit/test_pam.cpp
```
