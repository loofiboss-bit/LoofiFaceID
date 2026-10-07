# Test matrix

> The standard product is a Fedora 44/KDE local profile/comparison utility.
> v5.3.0 also contains a separate opt-in PAM
> experiment, which remains unsupported and unqualified for login. Automated
> checks do not qualify hardware, accessibility, PAD, or real authentication.

| Area | Automated evidence |
|---|---|
| Model supply chain | exact YuNet/SFace names, sizes, hashes, licenses, provenance; missing/renamed/modified/duplicate/unlisted rejection |
| OpenCV bridge | owned input copies, five-landmark alignment, 112×112 BGR crop, 1×128 FP32 feature, exception containment, malformed/non-finite rejection |
| M3 inference selection | OpenCV thread cap and 1/2/4/8/16 sweep schema, OpenVINO/Vulkan probing, forced accelerator-failure CPU fallback, 320×320 tracking, source-resolution detection, 1920×1080 bounds |
| M3 memory/sandbox | best-effort `mlock2(MLOCK_ONFAULT)`, zeroized native matrices/model copies, optional Landlock policy, opt-in DRM-filtered seccomp path |
| M3 quantization audit | offline model metadata, embedding parity, LFW/IJB-C aggregate FAR/FRR drift report; actual data-dependent result requires permissioned INT8 weights and datasets |
| Embeddings | zero norm, non-finite/range failure, deterministic L2 normalization and cosine, model/version binding |
| Identity protocol | closed operations, positive generations, exact bounds, malformed/trailing/oversized frames, fixed response types, no scores |
| Vault crypto | AES-GCM round trip, wrong key, tag/ciphertext/AAD tamper, nonce uniqueness, plaintext/key absence |
| Vault format | schema/UID/model/hash/format/dimension/normalization/sample validation, truncation/oversize, corruption preservation |
| Vault filesystem | owner/mode/type/symlink/hard-link checks, bounded locking, verified atomic write, rollback, rotation, deletion/reset |
| Enrollment | 3–8 bounds, duplicate rejection, one-frame actions, shared 300-second lifecycle, cancellation, no partial commit, failed replacement preserves the previous valid profile |
| Verification | match/no-match/ambiguous thresholds, median aggregation, no-profile/mismatch errors, cancellation, timeout, stale generations, rate limiting |
| KCM/QML | complete Home/Setup/Test/Diagnostics surface creates offscreen, 320/480/960 geometry, keyboard focus and accessibility names, destructive dialogs, page/app/preview teardown |
| Synthetic lifecycle | fake-worker camera discovery, preview, one-frame analysis, five-sample enrollment, atomic save, profile refresh, one-frame verification, clear, delete, and teardown repeated for 100 bounded cycles |
| Status/privacy | separate local identity capabilities, PAM/system states unsupported, aggregate-only support reports, no embeddings/scores in QML |
| Security boundary | standard build has no PAM/authselect writes, service, privilege, socket/network, runtime download, production fake selector, or arbitrary vault root; opt-in helper changes are limited to explicit administrator actions |
| Experimental auth setup | default artifacts absent; opt-in package contains intended components including isolated auth worker; helper target isolation, idempotency, failure injection, and rollback run against temporary state |
| Packaging | exact dependencies/files/licenses/workers, ordinary permissions, no auth scriptlets, reproducible archive/SRPM/RPM and isolated lifecycle |
| Package transition | v3 payload fixture is obsoleted by v4, old KCM files removed, user/PAM sentinels preserved, clean install/reinstall/remove deterministic |
| Release workflow | least privilege, exactly one source archive/binary RPM/SRPM, complete strict checksums, seven-day temporary retention, fail-closed published-release upload |
| Localization | all active user-visible messages have checked Swedish translations |
| M5 Liveness & PAD (experimental primitives only) | Internal threshold logic and fail-closed texture-analysis errors; no physical presentation studies, no qualified anti-spoofing system, no ISO/IEC claim |
| PAM qualification | `NOT RUN` unless recorded in `RELEASE-QUALIFICATION-V5.3.md`; isolated setup tests do not prove SDDM/Plasma login, real password fallback, or physical qualification |

Hardware, representative FAR/FRR, demographic/bias behavior, liveness, and
spoof resistance are deliberately separate qualification evidence. The
synthetic lifecycle checks state and worker cleanup only; it is not hardware or
authentication qualification.

## v5.3.0 regression coverage

- Shared preview/enrollment deadline, ordinary 60-second preview, single explicit
  300-second enrollment budget, guidance retry and stale observation rejection.
- Profile replacement confirmation, atomic failed-save preservation, actual
  keyboard focus and content geometry under narrow/large-text QML states.
- Auth-child hang/crash/malformed output, timeout/kill/reap/next healthy request,
  serialization, reserved fields, exact lengths and 269-byte socket ingress.
- Local/auth extraction-purpose separation and conservative auth heuristic errors.
- Native camera metadata fixtures, supported formats, metadata-node rejection,
  multiple-camera ambiguity and node numbers greater than 15.
- Profile-median evaluation with separate roles, failure denominators and >64
  manifest entries; no physical FAR/FRR or PAD result is implied.
- Source allowlist, deterministic gitless archive rebuild, version disagreement,
  tag/archive lineage and identical/missing/conflicting remote-asset fixtures.

## Reliability/recovery candidate regressions

- Real validated policy snapshots plus synthetic delayed workers: Off/restore,
  identical-content replacement, other-user policy changes and revocation do
  not allow a prior positive result or clear its retry count.
- Root-owned resync transactions: all independent mode pairs, all-Off/no-profile,
  another user's choices, corrupt staging/rollback data and exclusive policy
  locking. No host PAM/service/camera mutation is needed for these fixtures.
- KCM injected status/runner and synthetic identity worker: same-mode resync,
  wallet failure, authorization cancellation, duplicate clicks, missing/unknown
  readback, different target mode, timeout and discarded late results. Pending
  policy reads before enrollment commit cover completion, cancellation, unknown
  status and bounded timeout. Theme metadata reads reject non-regular files
  without blocking.
- QML: three real mode choices, Unknown display, saved-mode restoration,
  busy/last-error feedback, narrow-window geometry and keyboard focus.
- Reports: fixed per-target readiness/modes/issues and no untrusted mode text,
  identifiers, paths, credentials or biometric material.
- PAM and upstream Qt: allowlisted terminal categories, malformed positive
  responses, raw-text rejection and terminal failures that defeat late success.
- Native bridge: concurrent group lookups keep their own GIDs; child-file opens
  do not wait for FIFO writers. Root metadata suites also run in parallel.
- Integration workflow: exact pinned patches, default-OFF/opt-in builds, actual
  Qt tests, DESTDIR marker/worker paths and standard omission; pinned NoxForge
  source contracts remain distinct from runtime qualification.

## Local reliability verification, 2026-10-07

These results apply to the local reliability/recovery candidate working tree,
not to an installed package or published release.

| Gate | Result |
|---|---|
| LoofiFaceID Debug builds, standard / experimental auth | Passed, native sanitizers enabled |
| Isolated offscreen CTest, standard / experimental auth | 18/18 and 19/19 suites passed |
| Python repository tests | 73/73 passed |
| Rust workspace tests, offline and locked | Passed |
| Isolated root metadata/transaction suites | Templates 25/25, sync helper 14/14, policy CLI 2/2, daemon 20/20 passed |
| Parallel root helper stress | Five additional runs of 14/14 passed with 16 test threads |
| Pinned SDDM builds, standard / experimental | Both passed; 4/4 CTest each; 10 patch contracts passed |
| Pinned KScreenLocker builds, standard / experimental | Both passed; focused Qt suite passed; 12 patch contracts passed |
| Integration staged payloads | All four boundaries and six verifier regressions passed |
| LoofiFaceID staged payloads | Standard and experimental boundaries passed |
| Formatting, Clippy, QML lint, Swedish translations | Passed |
| Model supply chain, project identity, final diff whitespace | Passed |

The Qt fixtures cover enrollment waiting for policy completion, cancellation,
timeout, unknown status and ignored late callbacks. The filesystem fixtures
also reject FIFO configuration/child files without waiting for a writer.

All privileged transaction tests used disposable state; offscreen application
tests ran in containers without host PAM configuration, runtime sockets or
camera devices. Staged installs used temporary DESTDIR roots. No package was
installed on the host, no authentication target was activated, and no commit,
push or publication was performed. The GitHub workflow is prepared and locally
replayed; it has not run remotely for this working tree.

Physical SDDM login, Plasma unlock, password fallback, SELinux Enforcing,
suspend/resume, camera release and accessibility remain unqualified. Pinned
NoxForge checks establish source adapter contracts only; runtime theme
qualification remains open. SDDM theme test mode provides UI evidence and does
not execute login actions, as documented in the upstream
[theme guide](https://github.com/sddm/sddm/blob/v0.21.0/docs/THEMING.md).
