# LoofiFaceID Improvement Plan

- **Review date:** 2026-10-05
- **Review baseline:** `88ff73d` (`origin/main`, after PR #12)
- **Implementation status:** Security fixes and release-documentation updates
  are prepared on `codex/release-v5.2.0`. Candidate CI, merge, GitHub/COPR
  publication, and wiki readback are pending. Manual hardware qualification
  and real PAM login qualification remain open.
- **Scope:** Reliability, test coverage, documentation, packaging, and
  qualification.

## Current product boundary

The standard package is a logged-in-session utility for local face-profile
enrollment and explicit comparison. The optional PAM, daemon, systemd, and
SELinux components are experimental, off by default, and not qualified for
login. Preserve this separation while completing the work below. A local
comparison result is not authentication or presentation-attack evidence.

The README and package description state this boundary clearly. The next work
should close evidence and release-engineering gaps before adding recognition,
acceleration, or authentication features.

## Implementation record

- The current v5.2 status, version-neutral release checklist, KCM/PAM
  qualification split, and published v5.0.0 correction record are in place.
  The dated host readback was moved outside the source tree and package inputs.
- CI now builds and tests standard and opt-in configurations separately,
  checks both staged installs, and builds the experimental RPM subpackage in
  its own job. The ordinary RPM and COPR build remain authentication-free.
- The setup helper is exercised against a temporary filesystem and stubbed
  commands. Tests cover both PAM targets, idempotency, exact-rule adoption,
  password fallback, malformed markers, and failure rollback.
- The privileged helper emits fixed success status instead of caller/profile
  metadata. CI now fails before CTest and Python fixture tests unless the
  Fedora container runs as root, so root-only vault tests cannot silently
  return early in the canonical workflow.
- Targeted local Rust format, test, and Clippy checks pass for the helper. The
  desktop runs as UID 1000, so this does not execute the root-only ownership
  and rollback branches; candidate Fedora CI must provide that evidence.
- The v5.0.0 release text was corrected publicly on 2026-10-05; the original
  tag and four assets were preserved. The final correction record is in
  `docs/RELEASE-ERRATA-V5.0.0.md`.
- Candidate CI, stable v5.2.0 publication, COPR build, and wiki update remain
  pending until the exact published source and artifacts are read back.
- KCM manual hardware/accessibility qualification is `NOT RUN`; PAM login,
  Enforcing-mode SELinux, physical presentation-attack qualification, and
  independent security review remain `NOT RUN` or `OPEN`.

## Findings

1. **Resolved — project status was spread across documents with conflicting scope.**
   [`ROADMAP.md`](ROADMAP.md) points to the historical v5.0 roadmap and still
   describes PAM as blocked, while the source now contains a v5.2 opt-in
   experiment. [`RELEASE-CHECKLIST.md`](RELEASE-CHECKLIST.md) still identifies
   v5.1.0 as the current unreleased target.
2. **Resolved — the base RPM included host-specific qualification notes.** The RPM spec
   packages every `docs/*.md`; [`RELEASE-QUALIFICATION-V5.2.md`](RELEASE-QUALIFICATION-V5.2.md)
   includes local SELinux, service, and PAM configuration readbacks. These
   operational details do not belong in general user documentation or the
   standard package payload.
3. **Resolved — CI only built the default configuration.** The experimental components
   are controlled by `KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS`, which
   defaults to `OFF`. The CI and RPM workflows do not configure the `ON` path or
   build the experimental RPM subpackage, so changes to that path are not
   covered by the canonical workflow.
4. **Resolved — the privileged setup transaction lacked execution-level tests.** The SELinux
   policy tests check shell syntax and source text. The qualification report
   says the helper's rollback behavior has source-contract checks only and the
   privileged helper has not been executed.
5. **Open — important qualification remains outstanding.** SELinux policy activation, PAM
   login through SDDM and the Plasma lock screen, password fallback, device and
   session cycles, physical attack behavior, and independent security review
   have not been demonstrated. The opt-in path must remain experimental and
   disabled by default.

## Ordered plan

### 1. Make project status and shipped documentation consistent

**Priority:** P0
**Files:** `docs/ROADMAP.md`, `docs/RELEASE-CHECKLIST.md`,
`docs/RELEASE-QUALIFICATION-V5.2.md`, `packaging/fedora/kfaceauth.spec`,
`tests/test_packaging.py`

- Make `ROADMAP.md` the current, short status page. Link the v5.0 roadmap and
  v4 review as historical references, and state the actual v5.2 package and
  authentication boundaries.
- Replace the v5.1 release checklist with a version-neutral checklist or a
  v5.2 checklist that separates standard-package gates from experimental-auth
  gates.
- Keep qualification reports focused on reproducible evidence. Remove
  machine-specific host readbacks from distributable documentation; retain
  operational records outside the RPM documentation set.
- Replace `%doc docs/*.md` with an explicit allowlist of user and developer
  documentation. Add a package-content check that rejects qualification logs,
  private host state, and internal planning documents from the standard RPM.

**Done when:** the README, roadmap, checklist, qualification report, and RPM
description agree; every current link resolves; and the standard RPM contains
no machine-specific operational report.

### 2. Build and package both configurations in CI

**Priority:** P1
**Files:** `.github/workflows/ci.yml`, `.github/workflows/rpm.yml`,
`packaging/fedora/kfaceauth.spec`, `tests/test_packaging.py`

- Add an explicit default configuration job and an explicit experimental
  configuration job using `KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=ON`.
- Build and test the experimental RPM subpackage in a separate job. Keep it out
  of the normal COPR/default build path.
- Inspect staged install and RPM payloads in both jobs: the standard package
  must contain zero authentication artifacts; the opt-in subpackage must
  contain the intended PAM, daemon, service, setup, and policy artifacts.
- Run the existing C++, Rust, Python, QML, formatting, model, and packaging
  checks against the relevant build configuration.

**Done when:** a change to either configuration fails CI when it breaks, and
the workflow verifies both package boundaries from built artifacts.

### 3. Exercise setup and rollback as transactions

**Priority:** P1
**Files:** `data/pam/kfaceauth-pam-setup.sh`, `tests/test_selinux_policy.py`,
new helper integration fixtures

- Execute the helper against a disposable filesystem and stubbed
  `systemctl`, `semodule`, and `restorecon` commands. Do not edit the host's
  PAM, systemd, or SELinux state during automated tests.
- Cover enable/disable and repeat operations for SDDM and Plasma lock screen
  independently, including exact-rule adoption, malformed markers, missing
  password fallback, and unmanaged PAM rules.
- Inject failures after each state-changing step: SELinux module install or
  removal, PAM replacement, daemon reload, socket enable/disable, and relabel.
- Assert that rollback restores the prior PAM bytes, socket enabled/active
  state, daemon active state, and module set; also assert that operating on one
  target leaves the other target intact.

**Done when:** tests execute the real helper logic for success, idempotency,
failure, and rollback paths and do not rely on source-string assertions as the
only evidence.

### 4. Close local-session hardware and accessibility evidence

**Priority:** P2
**Files:** `docs/HARDWARE-QUALIFICATION.md`, `docs/TEST-MATRIX.md`,
`docs/USER-GUIDE.md`, `src/kcm/ui/`

- Execute the documented preview-release procedure and record manual results
  for the ordinary enrollment and local-comparison workflow separately from
  PAM qualification.
- Exercise camera selection and recovery, cancellation, suspend/resume, and
  repeated enrollment/test sessions on the supported Fedora 44 and Plasma 6
  baseline.
- Review keyboard-only operation, focus order, narrow-window scaling, and
  screen-reader labels. Turn observed failures into focused regressions before
  changing UI structure.
- Report only the tested device, lighting, desktop, and accessibility
  conditions; keep untested combinations marked `unverified`.

**Done when:** the supported local workflow has a reproducible manual matrix
and identified failures have regression coverage or a documented limitation.

### 5. Decide whether to pursue supported system authentication

**Priority:** P3; starts only after steps 1–3 and an explicit product decision
**Files:** `docs/RELEASE-QUALIFICATION-V5.2.md`,
`docs/THREAT-BOUNDARY.md`, PAM/daemon/SELinux implementation

- Obtain an independent review of PAM ordering, UID binding, key separation,
  attempt limits, service confinement, SELinux policy, and rollback behavior.
- On Fedora 44 with SELinux Enforcing, activate the packaged policy through the
  documented path and verify labels, service startup, and relevant audit events.
- If the feature remains a product goal, complete repeated SDDM and Plasma
  lock-screen cycles, positive and negative decisions, and password fallback
  after camera, profile, timeout, daemon, and non-match failures. Include
  missing/busy camera, suspend/resume, and multiple-user cases.
- Complete the consent-based physical evaluation and aggregate-only reporting
  already specified in the qualification document. Do not use camera spectrum,
  image quality, or an internal match threshold as liveness evidence.
- If representative physical testing and independent review are not available,
  keep the path off by default and describe it only as an unsupported experiment.

**Done when:** every release gate has independently reviewable evidence and the
product decision, package contents, and user-facing claims agree. No gate is
waived by a green CI run.

## Explicitly deferred

Do not add new model families, accelerator backends, passive camera operation,
or authentication targets as part of this plan. Revisit them only after the
reliability and qualification work establishes a concrete user need and a
measurable acceptance target.

## Review basis

- [`README.md`](../README.md) and
  [`RELEASE-QUALIFICATION-V5.2.md`](RELEASE-QUALIFICATION-V5.2.md): current
  product boundary and open qualification gates.
- [`ROADMAP.md`](ROADMAP.md) and
  [`RELEASE-CHECKLIST.md`](RELEASE-CHECKLIST.md): outdated status references.
- [`ci.yml`](../.github/workflows/ci.yml),
  [`rpm.yml`](../.github/workflows/rpm.yml), and
  [`kfaceauth.spec`](../packaging/fedora/kfaceauth.spec): separate standard and
  opt-in build/package paths with explicit documentation and payload checks.
- [`test_pam_setup_transaction.py`](../tests/test_pam_setup_transaction.py):
  the real setup helper runs against temporary PAM, service, and SELinux
  fixtures with injected command failures.
