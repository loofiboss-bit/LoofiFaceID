# v5.2 release-series qualification

**Stable package scope:** explicit local-profile enrollment and comparison in
an already logged-in session. Stable package delivery is not biometric,
authentication, or presentation-attack qualification.

The published v5.2.1 GitHub release is the current stable Fedora 44 package
release. It corrects the v5.2.0 COPR rebuild failure by declaring the standard
`systemd-devel` dependency for `libudev`. The v5.2.1 tag and release workflows
passed, and all four published assets passed checksum and payload verification.
The exact published SRPM succeeded in [COPR build 11075253](https://copr.fedorainfracloud.org/coprs/build/11075253/),
and the public Fedora 44 repository lists `kfaceauth-5.2.1-1.fc44`.

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
| Build and package boundary | Standard and opt-in CMake builds; standard RPM excludes auth artifacts; experimental RPM contains only the opt-in components | The v5.2.1 tag passed [Fedora 44 CI](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37264564300) and [RPM checks](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37264564276). The release-triggered [CI](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37265108773) and [RPM, lifecycle, and artifact upload](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37265108797) also passed. The source archive, standard RPM, SRPM, and `SHA256SUMS` were downloaded and verified; the standard RPM contains no authentication payload. |
| COPR rebuildability | Fedora 44 x86_64 standard package builds from the published SRPM | v5.2.0 COPR build [11075135](https://copr.fedorainfracloud.org/coprs/build/11075135/) failed at CMake configuration because `libudev.pc` was unavailable. v5.2.1 makes `systemd-devel` a standard `BuildRequires`; the exact published SRPM passed [COPR build 11075253](https://copr.fedorainfracloud.org/coprs/build/11075253/), and public repository metadata lists `kfaceauth-5.2.1-1.fc44`. |
| Helper, vault, and deadline regressions | Fixed helper roots and caller UID, protected vault metadata, rollback/replay resistance, two-second deadline, bounded ingress, and password fallback | Rust workspace tests and root-only vault ownership/rollback cases passed in the Fedora CI container. The workflow fails unless the test process is root. This desktop runs as UID 1000, so root-only ownership and rollback cases return early locally. |
| Setup transaction | Target isolation, idempotency, PAM preservation, command failure injection, rollback of PAM/service/policy state | Local isolated execution tests pass; they do not touch host PAM or SELinux |
| SELinux integration | Fedora 44 Enforcing; expected labels, service startup, and relevant audit events | `NOT RUN` for the login path |
| Real login paths | Repeated SDDM and Plasma lock-screen decisions with password fallback after every failure | User-reported on 2026-10-05: face authentication worked for both SDDM login and Plasma lock-screen unlock. The tested build/device, attempt counts, negative cases, and password-fallback behavior were not recorded. Full repeated-login and failure-fallback qualification remains `NOT RUN`. |
| Device/session behavior | At least 20 cycles on at least two RGB cameras and three lighting conditions; missing/busy camera, suspend/resume, and multiple users | `NOT RUN` |
| Physical attack behavior | Consent-based RGB and IR evaluation with aggregate-only reporting and the thresholds below | `NOT RUN` |
| Independent review | PAM ordering, UID/key separation, attempt limits, service confinement, SELinux policy, rollback, and failure behavior | Post-patch source review: `PASS`; all [CodeQL analyzers for the v5.2.1 merged source](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37264532110) passed. System/runtime and physical qualification remain `NOT RUN`. |

The v5.2.1 release-tag standard and opt-in builds, RPM payload checks, release
workflows, and published asset upload passed. The opt-in RPM was built and
inspected but is not a release asset or part of COPR. The standard release RPM
has no PAM module, daemon, authentication units, setup helper, or SELinux
policy. The four-file release manifest was downloaded and verified against
`SHA256SUMS`; its source archive includes the published v5.0.0 correction
record and excludes internal planning material. COPR build 11075253 succeeded
from the exact published SRPM, and the public Fedora 44 repository lists the
resulting standard package.

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
