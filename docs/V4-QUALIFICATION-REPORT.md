# KFaceAuth v4.0.0 qualification report

Status: `NOT RUN`

Copy this template for each controlled release-candidate qualification run.
Keep every manual field as `NOT RUN` or `UNQUALIFIED` until directly observed.
Automated results do not substitute for physical hardware, accessibility, or
participant qualification.

## Record

- Qualification date (UTC): `NOT RUN`
- Tester or test-team alias: `NOT RUN`
- Tested commit (full SHA-1): `NOT RUN`
- Fedora version: `NOT RUN`
- Kernel version: `NOT RUN`
- CPU model class: `NOT RUN`
- OpenCV version: `NOT RUN`
- KFaceAuth version / RPM NEVRA: `NOT RUN`
- Camera class, without serial number or stable device path: `NOT RUN`
- Consent scope and retention agreement: `NOT RUN`

## A. Experimental preview release readiness

The fields below cover only the logged-in preview utility. Keep each manual
field as `NOT RUN` until directly observed; automated evidence must be listed
separately and must not be turned into physical qualification.

- RGB camera coverage: `NOT RUN`
- Infrared camera coverage: `NOT RUN`
- Enrollment behavior (3 minimum, 5 recommended, 8 maximum): `NOT RUN`
- Correct-person aggregate result categories: `UNQUALIFIED`
- Consenting wrong-person aggregate result categories: `UNQUALIFIED`
- Cancellation and teardown behavior: `NOT RUN`
- Locked/cancelled/unavailable KWallet behavior: `NOT RUN`
- Permanently lost KWallet key behavior: `NOT RUN`
- Corrupt/truncated/model-mismatched vault behavior: `NOT RUN`
- Delete and explicit unreadable-reset behavior: `NOT RUN`
- Keyboard-only navigation at 320, 480, and 960 pixels: `NOT RUN`
- Assistive-technology behavior and focus return after dialogs: `NOT RUN`
- Cold latency (first fresh worker, aggregate only): `NOT RUN`
- Warm latency (median/p95/worst, aggregate only): `NOT RUN`
- Peak resident memory (parent and worker, aggregate only): `NOT RUN`
- UI responsiveness and 20-cycle teardown result: `NOT RUN`

## B. Authentication suitability — UNQUALIFIED

These are not v4.0.0 release-readiness checks and must remain explicitly
unqualified:

- PAM, authselect, SDDM, lock-screen, sudo, Polkit, or system authorization:
  `UNQUALIFIED`
- pre-login key access outside the logged-in KWallet session: `UNQUALIFIED`
- representative FAR/FRR and wrong-person acceptance: `UNQUALIFIED`
- demographic/bias behavior: `UNQUALIFIED`
- liveness and presentation-attack/spoof resistance: `UNQUALIFIED`
- authentication suitability or security tier: `UNQUALIFIED`

## Outcome

- Tested coverage: `NOT RUN`
- Untested coverage: `UNQUALIFIED`
- Failures: `NOT RUN`
- Remaining release blockers: `UNQUALIFIED`
- Overall qualification decision: `UNQUALIFIED`

Passing Section A or local comparison checks does not qualify Linux
authentication, liveness, presentation-attack detection, spoof resistance,
FAR, FRR, bias, or demographic behavior. Record those areas as `UNQUALIFIED`;
do not infer them from RGB/IR class, brightness, contrast, sharpness, movement,
or other image-quality signals.

## Privacy rules

Never store in this report or the repository:

- participant names or stable participant identifiers;
- images, screenshots, frames, or image paths;
- camera serial numbers, stable device paths, or persistent hardware IDs;
- embeddings, landmarks, plaintext templates, encryption keys, or vault paths;
- individual comparison scores or per-participant/per-image results.

Use only aggregate result categories and anonymous, transient counts. Link no
external dataset unless its consent, storage, and redistribution terms have
been separately reviewed.
