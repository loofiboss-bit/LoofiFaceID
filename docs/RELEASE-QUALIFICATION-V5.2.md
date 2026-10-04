# v5.2.0 authentication experiment qualification

**Status: opt-in experiment; not supported or qualified for login.** The
ordinary CMake build and COPR RPM omit all authentication components. An
experimental RPM subpackage is available only when explicitly requested.

## Product boundary

The project targets Fedora 44 with KDE Plasma 6. The logged-in-session KCM
continues to use the user's KWallet profile for local comparison. Experimental
pre-session authentication uses a separate system vault and a separately
generated key. PAM integration can be selected independently for SDDM and the
Plasma lock screen. `auth sufficient pam_kfaceauth.so` returns success only
after an explicit positive daemon response; module errors, timeout, missing
camera, missing or invalid profile, and non-match return control to the
existing PAM stack so its password path remains available.

The experiment does not configure `sudo` or Polkit as authentication factors.
Administrator authorization is used only for explicit system setup. It is
never enabled by package installation.

## Build and package boundary

- `KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF` is the only default CMake
  setting. It excludes PAM, daemon, vault synchronization, systemd units,
  sysusers, Polkit setup action, and SELinux policy artifacts.
- The normal `kfaceauth` RPM contains only the local-profile application.
- `rpmbuild --with experimental_auth` creates a separate
  `kfaceauth-experimental-auth` RPM that contains the opt-in authentication
  components. COPR builds use the default, authentication-free package.
- Installing either package does not edit PAM or start/enable the service.

## SELinux AVC investigation

The Fedora 44 host is in Enforcing mode. The screenshot's setroubleshoot view
attributes `{ mounton }` on
`/run/systemd/mount-rootfs/run/kfaceauth` to `kfaceauthd`; the earlier host AVC
inspection identified the denied process type as `init_t` and the target type
as `kfaceauth_sddm_sock_t`. Read-only journal output shows systemd failing to
mount `/run/kfaceauth` onto its service mount namespace and exiting at
`NAMESPACE` with status 226. The original raw event and syscall were not
preserved in the available evidence, so its full AVC record remains
unverified.

## Latest host readback (2026-10-04)

The locally built `kfaceauth` and `kfaceauth-experimental-auth` RPMs are both
version `5.2.0-1.fc44`; RPM verification is clean. These are unsigned local
build artifacts, not release packages. The host remains in SELinux Enforcing
mode. The updated daemon and socket are active, the socket is enabled for
socket activation, and a read-only `OP_STATUS` request returned `ready`.
The service starts with the new unit, which no longer mounts `/run/kfaceauth`
into its private writable namespace. A recent privileged `ausearch` query
returned no AVC records, but no SDDM or lock-screen authentication attempt was
made, so this does not qualify the SELinux integration or prove the original
AVC cannot recur in the login path.

The new SELinux policy files are present in the experimental package but were
not loaded with `semodule`; the privileged KCM setup helper was not run. The
active policy still contains `kfaceauth_sddm` at priority 400, and
`/run/kfaceauth` and its socket still have the old
`kfaceauth_sddm_sock_t` label. The source policy instead labels only the exact
socket pathname and leaves the parent directory at the platform default.
Policy activation and label restoration therefore remain unverified.

Before this package installation, both `/etc/pam.d/sddm` and `/etc/pam.d/kde`
already contained an `auth sufficient pam_kfaceauth.so` line followed by the
`password-auth` substack. The `password-auth` stack contains `pam_unix.so`.
Installation left both PAM files byte-for-byte unchanged; their hashes before
and after installation match. These existing rules are not evidence that a
face login or password fallback works.

The service uses `ProtectSystem=strict`. Because `kfaceauth.socket` creates and
owns the listener and passes its file descriptor to the daemon, the daemon
does not need a writable bind mount for `/run/kfaceauth`. The installed unit
keeps only `/var/lib/kfaceauth` in `ReadWritePaths`; this removes the
unnecessary runtime-directory mount from the daemon namespace. These changes do
not add a `mounton` permission or weaken SELinux.

The source policy gives the custom type only to the exact socket pathname and
leaves `/run/kfaceauth` at the platform default label. The separate SDDM rule
grants access only to writing that socket and connecting to the socket-
activation domain. Activation installs the prebuilt modules and restores the
runtime labels before editing PAM. No `mounton` permission, broad
`audit2allow` output, or SELinux mode change is added; the setup helper does
not compile policy on the target host.

The new systemd unit and packages are installed and the daemon is running, but
the PAM setup helper and new SELinux policy have not been activated. No login
path or post-activation AVC qualification has been demonstrated.

## Qualification gates

| Gate | Requirement | Status |
|---|---|---|
| Automated behavior | Default and opt-in build/package contents; key separation; positive-only PAM success; password fallback; timeout; rate limiting; transactional target setup | **Code checks passed:** 48 Python tests, default CTest 16/16, opt-in CTest 17/17, Rust tests 79/79; staged install had 0 authentication artifacts by default and all 9 opt-in artifacts. After the final blocked-target recovery change, focused CTest passed 2/2 default and 3/3 opt-in. Helper rollback behavior has source-contract checks only; the privileged helper was not executed. |
| AVC investigation | Reproduce under Enforcing and record process/object contexts, class, denied permission, and syscall | **Open:** screenshots and journal identify the `mounton` failure; the complete original AVC and syscall were not preserved, and no reproduction was performed |
| SELinux integration | Fedora 44, Enforcing; verify expected labels, service startup, and no relevant AVCs | **Partial:** new daemon unit starts and a recent audit query returned no AVC records; old policy/labels remain active because the helper was not run, and the login socket path was not exercised |
| Real login paths | Repeated login and unlock through SDDM and Plasma lock screen; verify password fallback after every face failure | **Not run:** PAM files already contain the experimental module and password substack, but no face decision or fallback was tested |
| Device and session behavior | At least 20 cycles on at least two RGB cameras and three lighting conditions; missing/busy camera, suspend/resume, multiple users, password fallback | **Not run** |
| Attack and error behavior | Documented consent-based spoof and wrong-person testing; evaluate RGB and IR separately; report aggregate results only | **Not run** |
| Independent review | Review PAM ordering, UID binding, key separation/storage, setup rollback, SELinux policy, and failure behavior | **Open:** source diff scan found no reportable issue; this is not an independent external security or biometric qualification, and privileged setup/rollback remains untested |

Before support status, physical attack testing must include at least 300
consent-based photo presentations and 300 screen-replay presentations per
camera spectrum, with APCER no greater than 1% for each attack type. Bona-fide
testing must reach BPCER no greater than 5%, and wrong-person comparisons must
cover at least 3,000 attempts with FMR no greater than 0.1%. RGB and IR results
must be reported separately; each metric must include sample counts and
aggregate outcomes without retaining face images or embeddings.

IR camera selection is only device selection. It provides no liveness,
presentation-attack detection, or spoof-resistance evidence. Enrollment quality,
detector output, and an internal match threshold are not substitutes for those
tests. Use only consent-based material and retain aggregate results without
face images or embeddings.

Until every gate is independently completed, this feature must remain
experimental, off by default, and absent from the ordinary package.
