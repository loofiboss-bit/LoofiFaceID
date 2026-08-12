# Hardware qualification

Automated and build-host measurements are not camera, demographic, FAR/FRR,
liveness, spoof, or authentication qualification. Milestone 4 may pass code
gates without hardware evidence, but missing hardware coverage is a release
blocker and must be reported.

Qualification has two intentionally separate sections:

- **A. Experimental preview release readiness** covers camera behavior,
  enrollment, cancellation and teardown, KWallet/vault recovery, keyboard and
  Orca behavior, aggregate latency/memory, and privacy inspection.
- **B. Authentication suitability** covers PAM/system authentication,
  pre-login key access, FAR/FRR, demographic/bias behavior, liveness, and spoof
  resistance. Section B is permanently `UNQUALIFIED` for v4.0.0 and is not
  enabled by passing Section A.

## Non-installed evaluator

Use only images whose participants explicitly consented to this evaluation and
whose storage/license terms permit local processing. Do not add the dataset,
manifest, results with identities, screenshots, or camera frames to the
repository.

The manifest is tab-separated: an opaque transient group number followed by an
absolute path to a binary PPM (`P6`) image. Group numbers are used only in
memory for same/different aggregate comparisons and are never emitted.

```bash
cargo run --manifest-path engine/Cargo.toml --release --offline \
  --bin kfaceauth-identity-evaluate -- \
  --model-root "$PWD/models" \
  --dataset-manifest /private/consented-evaluation.tsv
```

JSON output contains only environment, sample/error counts, model
initialization, fresh-provider and reused-provider pipeline
median/p95/worst, first and subsequent fresh-worker-process latency, parent
and worker peak resident memory, aggregate repeated-sample consistency,
aggregate same/different comparisons, and aggregate threshold sweep counts.
The first worker observation is the closest available cold-start measurement;
subsequent fresh processes normally benefit from the operating-system page
cache. Repeated truly cold measurements require an externally controlled
qualification host. The tool never emits an image, embedding, landmark,
participant/group label, path, persistent camera identifier, or per-image
score.

FAR/FRR fields are `unqualified` unless the caller explicitly adds
`--labelled-evaluation` and supplies at least two identities with at least two
accepted samples each. Even then, results describe only that supplied set and
are not product qualification.

## A. Experimental preview release readiness

1. Use a clean, normally dependency-resolved Fedora 44 installation. Record
   Fedora/kernel/CPU/OpenCV/KFaceAuth versions and RGB/IR class without serial
   number or stable device node.
2. Unlock KWallet, start preview explicitly, start enrollment explicitly, and
   capture every sample with a separate click. Confirm 3 samples enable Finish,
   5 are recommended, 8 is the hard bound, and no continuous capture occurs.
3. Cancel at each stage; hide the page; deactivate System Settings; stop
   preview; replace an active request; wait beyond 120 seconds; and close the
   KCM. Confirm no partial profile, lingering worker, frame, or result.
4. Complete enrollment and repeat explicit one-frame verification under
   ordinary glasses/appearance changes, modest pose, and varied ordinary
   lighting. Record only result categories and aggregate timing.
5. With separately consenting participants, run deliberate wrong-person
   checks. Never retain names, images, per-image scores, or claim FAR/FRR from
   a small convenience sample.
6. Lock KWallet and cancel its access prompt. Confirm stable unavailable
   results and no key-file fallback. Unlock and retry.
7. On an isolated disposable profile, modify/truncate the vault and lose the
   KWallet key. Confirm fail-closed unreadable state, corruption preservation,
   explicit destructive reset, and required re-enrollment.
8. Delete the valid profile, confirm status becomes absent, and re-enroll.
   Confirm deletion makes no physical-erasure claim.
9. Repeat preview/enrollment/test teardown at least 20 times. Confirm workers
   exit and the UI remains responsive.
10. Record CPU, model initialization, cold/warm median/p95/worst latency, peak
    RSS, fan/power behavior, and UI responsiveness. Inspect journal and
    redacted support report for images, keys, embeddings, scores, rectangles,
    identifiers, and user paths.

The automated fake-worker lifecycle complements this procedure with 100
synthetic start/stop and enrollment/test/teardown cycles. It does not replace
physical camera, keyboard, Orca, or privacy inspection.

## B. Authentication suitability — UNQUALIFIED

Do not run or report these items as v4.0.0 preview qualification:

- PAM, authselect, SDDM, lock-screen, sudo, Polkit, or any system
  authorization integration;
- pre-login key access or a credential provider outside the logged-in KWallet
  session;
- FAR/FRR, representative wrong-person acceptance, demographic/bias
  evaluation, liveness, presentation-attack, or spoof-resistance claims.

These capabilities are outside the product boundary. They remain
`UNQUALIFIED` and blocked regardless of RGB/IR results, image-quality guidance,
synthetic cycles, or a local `Match` result.

## Result record and release boundary

Record date, tester, environment, consent scope, camera class, conditions,
aggregate results, cancellation/teardown behavior, performance, failures, and
untested coverage. Do not record biometric material or stable participant
identity. Start each run from
[V4-QUALIFICATION-REPORT.md](V4-QUALIFICATION-REPORT.md) and leave every
unobserved field as `NOT RUN` or `UNQUALIFIED`.

Missing broad RGB/IR hardware and accessibility evidence blocks Section A.
Section B remains `UNQUALIFIED` by design. Passing Section A does not make
KFaceAuth suitable for authentication and does not authorize liveness, PAM, or
system-integration work.
