# Hardware and accessibility qualification

Automated tests and build-host measurements do not qualify camera behavior,
accessibility, FAR/FRR, liveness, spoof resistance, or authentication. Keep the
logged-in-session KCM and experimental PAM evidence in separate records.

Do not add datasets, participant identities, camera frames, embeddings,
screenshots, or per-image results to the repository. Use only consented
material, report aggregate results, and mark unobserved cases `NOT RUN` or
`unverified`.

## Current record

- KCM manual hardware and accessibility qualification: `NOT RUN` for this
  implementation candidate.
- PAM login-path, Enforcing-mode SELinux, and physical presentation-attack
  qualification: `NOT RUN`.
- User-reported on 2026-10-05: face authentication worked for both SDDM login
  and Plasma lock-screen unlock. The tested build/device, attempt counts,
  negative cases, and password-fallback behavior were not supplied. This
  limited report is not a complete or independently reviewed qualification;
  the full PAM track remains `NOT RUN`.
- Local targeted source checks pass. Root-owned vault metadata and rollback
  cases require root and are `NOT RUN` on this UID 1000 desktop; the candidate
  Fedora CI now fails early unless its test process is root.
- Root-owned vault metadata, replay, and rollback regressions passed in the
  Fedora 44 candidate CI run. That does not qualify SELinux, PAM login, cameras,
  accessibility, or physical attack behavior.
- Independent post-patch source review: `PASS`; root-only ownership/rollback
  execution, SELinux Enforcing, real PAM login, and hardware qualification
  remain `NOT RUN`.

## A. KCM local-session workflow

Run on Fedora 44 with KDE Plasma 6 using the standard, authentication-free
package. Record package, Fedora, Plasma, OpenCV versions, camera class and
tested lighting conditions. Do not record camera serial numbers or stable
device paths.

1. Test automatic selection with one usable camera and explicit selection with
   multiple usable cameras.
2. Exercise camera unavailable/busy recovery, preview start/stop, enrollment,
   explicit save, one-frame comparison, and profile deletion/re-enrollment.
3. Cancel at each enrollment and comparison stage; hide the page, close the KCM,
   suspend/resume, and replace an active request. Confirm workers exit and no
   partial profile or result is committed.
4. Repeat preview, enrollment, and comparison teardown at least 20 times.
5. Review keyboard-only operation, focus order, screen-reader names, narrow
   windows, and supported scaling.
6. Inspect diagnostics and support reports for images, embeddings, keys,
   scores, stable device identifiers, and private paths.

Record each scenario as `PASS`, `FAIL`, or `NOT RUN`, with reproducible steps,
conditions, and a concise result. Add regression coverage for observed defects;
document an unsupported combination when a defect cannot be resolved.

## B. Experimental PAM login workflow

This track remains **unqualified and unsupported for login** until every gate
below has independent evidence. Do not run it on a daily-use desktop. Use a
separate Fedora 44 qualification machine with SELinux Enforcing, a recovery
account/console, and consented participants.

### Prerequisites

- Default and opt-in builds and RPM contents pass their CI checks.
- The setup helper's success, idempotency, failure, and rollback paths pass in
  an isolated test environment.
- An independent reviewer has examined PAM ordering, UID binding, key
  separation/storage, attempt limits, service confinement, SELinux policy, and
  failure recovery.

### System and device coverage

1. Activate the packaged policy through the documented helper. Verify expected
   socket labels, service startup, and relevant audit events without changing
   SELinux mode or adding broad policy permissions.
2. Exercise repeated positive and negative SDDM login and Plasma lock-screen
   unlock attempts. Verify password fallback after non-match, missing/busy
   camera, missing/invalid profile, timeout, daemon failure, and suspend/resume.
3. Complete at least 20 device/session cycles across at least two RGB cameras
   and three lighting conditions; include multiple users and camera recovery.
4. Test RGB and IR separately for at least 300 consent-based photo presentations
   and 300 screen-replay presentations per spectrum. Require APCER no greater
   than 1% per attack type, BPCER no greater than 5%, and at least 3,000
   wrong-person comparisons with FMR no greater than 0.1%.

Publish aggregate counts and conditions only. Do not retain biometric material
or treat camera spectrum, image quality, or an internal match threshold as
liveness evidence. If any gate is unavailable or fails, retain the unsupported,
off-by-default status.

## Result record

For each track, record date, tester/reviewer role, build, Fedora/Plasma/OpenCV
versions, camera class, lighting and accessibility conditions, scenario,
result, failures, and untested coverage. Never record a participant's identity,
camera serial, image, embedding, or per-image score.
