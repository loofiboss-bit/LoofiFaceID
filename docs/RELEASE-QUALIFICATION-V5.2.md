# v5.2.0 release and authentication experiment qualification

**Release status: stable package release for the logged-in local-profile
workflow.** Stable describes the release channel and package delivery; it is
not biometric, authentication, or presentation-attack qualification.

**PAM status: opt-in experiment; not supported or qualified for login.** The
ordinary CMake build and COPR RPM omit all authentication components. The
experimental RPM subpackage is created only by an explicit build request.

## Product boundary

The logged-in-session KCM uses the user's KWallet profile for explicit local
comparison. The experimental pre-session path uses a separate system vault and
key, and can be selected independently for SDDM and the Plasma lock screen. PAM
success requires an explicit positive daemon response; errors and non-matches
return control to the remaining stack, which must retain a working password
path.

The experiment does not configure sudo or Polkit as authentication factors.
Package installation does not edit PAM or enable a service. The default build
and normal package contain no PAM, daemon, systemd, sysusers, Polkit, or SELinux
artifacts.

## Qualification evidence

Automated checks establish source/build behavior and package boundaries only.
They do not prove a real login, password fallback on a physical machine, SELinux
integration under an exercised login path, or biometric suitability. Use the
[release checklist](RELEASE-CHECKLIST.md) for build and artifact requirements
and [hardware qualification](HARDWARE-QUALIFICATION.md) for separate manual
tracks.

| Gate | Requirement | Status |
|---|---|---|
| Build and package boundary | Standard and opt-in CMake builds; standard RPM excludes auth artifacts; experimental RPM contains only the opt-in components | PR #13 implementation commit `5fa0e1e` passed [Fedora 44 CI](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37260509918) and [Fedora 44 RPM checks](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37260509976), including standard package install/removal lifecycle and both payload boundaries. Release-tag workflows remain a separate publication gate. |
| Helper, vault, and deadline regressions | Fixed helper roots and caller UID, protected vault metadata, rollback/replay resistance, two-second deadline, bounded ingress, and password fallback | Rust workspace tests and root-only vault ownership/rollback cases passed in the Fedora CI container. The workflow fails unless the test process is root. This desktop runs as UID 1000, so root-only ownership and rollback cases return early locally. |
| Setup transaction | Target isolation, idempotency, PAM preservation, command failure injection, rollback of PAM/service/policy state | Local isolated execution tests pass; they do not touch host PAM or SELinux |
| SELinux integration | Fedora 44 Enforcing; expected labels, service startup, and relevant audit events | `NOT RUN` for the login path |
| Real login paths | Repeated SDDM and Plasma lock-screen decisions with password fallback after every failure | `NOT RUN` |
| Device/session behavior | At least 20 cycles on at least two RGB cameras and three lighting conditions; missing/busy camera, suspend/resume, and multiple users | `NOT RUN` |
| Physical attack behavior | Consent-based RGB and IR evaluation with aggregate-only reporting and the thresholds below | `NOT RUN` |
| Independent review | PAM ordering, UID/key separation, attempt limits, service confinement, SELinux policy, rollback, and failure behavior | Post-patch source review: `PASS`; all [CodeQL analyzers](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37260508417) passed. System/runtime and physical qualification remain `NOT RUN`. |

For PR #13, `rpmlint` reported zero errors. It reported three warnings for the
standard build and five for the opt-in build: the Fedora container's missing
`en_US.UTF-8` locale, the desktop file's external `systemsettings` launcher,
and missing manual/documentation metadata for the opt-in setup helper. The RPM
workflow completed successfully. The exact release-tag builds, final asset
manifest, and publication upload must still pass and be read back separately.

Before any support status, physical testing must include at least 300
consent-based photo presentations and 300 screen-replay presentations per
camera spectrum, with APCER no greater than 1% per attack type. Bona-fide
testing must reach BPCER no greater than 5%, and wrong-person comparisons must
cover at least 3,000 attempts with FMR no greater than 0.1%. Report RGB and IR
separately with sample counts and aggregate outcomes; retain no images or
embeddings.

IR camera selection is device selection only. It provides no liveness,
presentation-attack detection, or spoof-resistance evidence. Image quality,
detector output, and an internal match threshold are not substitutes for the
required physical evaluation.

Until every gate is independently completed, the feature remains experimental,
off by default, and absent from the ordinary package and support claims.
