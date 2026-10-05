# v5.3.0 release qualification

**Stable package scope:** guided local profile enrollment and explicit
comparison in an already logged-in Fedora 44/KDE Plasma 6 session. Stable
release delivery does not qualify face comparison for login, liveness,
presentation-attack detection, or any security decision.

**PAM status:** an opt-in engineering experiment only. It remains unsupported
and unqualified for SDDM or Plasma lock-screen login. The ordinary build and
COPR package do not contain the PAM module, daemon, authentication worker,
systemd units, or SELinux policy. Password fallback is preserved in source but
has not been qualified against every failure on physical hardware.

## Automated release evidence

The v5.3.0 source candidate was checked locally on Fedora 44 and the project
build host:

- Standard CMake configuration: **17/17 CTest suites passed**.
- Opt-in experimental-auth CMake configuration: **18/18 CTest suites passed**.
- Python suite: **72 tests passed**. Rust workspace tests, Clippy with warnings
  denied, Cargo formatting, C++ formatting, QML lint, Swedish translation
  checks, and all six model-integrity checks passed.
- Exact v5.3.0 RPM/SRPM builds, payload inspection, `rpmlint`, lifecycle checks,
  artifact verification, and release workflow results are pending the tag CI.
  The locally verified isolated Fedora RPM build and artifact readback were for
  v5.2.1 and do not qualify these release-version artifacts.
- The release artifact verifier identifies Fedora source RPMs by the RPM
  `SOURCEPACKAGE` marker, independently of build architecture; a regression
  test covers the source-package marker.

These checks exercise source behavior and package contents. The automated
release workflows and public asset readback are separate publication checks;
a tag or local build alone is not proof that those completed.

## Product performance and biometric evidence

A synthetic 320x320 YuNet inference run on the build host measured 1,851.821 ms
cold, 60.503 ms warm median, 71.371 ms p95, and 66,636 KiB peak RSS. This is
not camera capture, full enrollment, face-match, login, or hardware latency.
No consent-based genuine/impostor dataset was evaluated for this release, so
there are no FAR, FRR, demographic, liveness, or spoof-resistance results.

## Manual qualification status

| Gate | v5.3.0 status |
|---|---|
| Physical camera recovery, enrollment, comparison, repeat sessions, suspend/resume | `NOT RUN` |
| Keyboard, large-text, screen-reader and assistive-technology review | `NOT RUN` |
| SELinux Enforcing behavior on the exercised PAM login path | `NOT RUN` |
| Repeated SDDM and Plasma lock-screen decisions on a qualification machine | `NOT RUN` |
| Password fallback after each authentication failure condition | `NOT RUN` |
| Consent-based physical presentation-attack and multi-camera evaluation | `NOT RUN` |
| Independent runtime qualification of the isolated authentication worker | `NOT RUN` |

The user's previously reported successful SDDM and Plasma unlock is limited
smoke evidence only. Tested build identity, attempt counts, negative decisions,
and fallback cases were not recorded. It does not change the `NOT RUN` gates
above. Unobserved results remain `unverified`; the PAM experiment must not be
described as supported or qualified.

Release qualification is limited to the local-session product, software
behavior, build, and package boundaries documented here. It does not establish
physical-device, accessibility, or biometric security suitability.
