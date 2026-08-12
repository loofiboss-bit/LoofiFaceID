# Test matrix

| Area | Automated evidence |
|---|---|
| Model supply chain | exact YuNet/SFace names, sizes, hashes, licenses, provenance; missing/renamed/modified/duplicate/unlisted rejection |
| OpenCV bridge | owned input copies, five-landmark alignment, 112×112 BGR crop, 1×128 FP32 feature, exception containment, malformed/non-finite rejection |
| Embeddings | zero norm, non-finite/range failure, deterministic L2 normalization and cosine, model/version binding |
| Identity protocol | closed operations, positive generations, exact bounds, malformed/trailing/oversized frames, fixed response types, no scores |
| Vault crypto | AES-GCM round trip, wrong key, tag/ciphertext/AAD tamper, nonce uniqueness, plaintext/key absence |
| Vault format | schema/UID/model/hash/format/dimension/normalization/sample validation, truncation/oversize, corruption preservation |
| Vault filesystem | owner/mode/type/symlink/hard-link checks, bounded locking, verified atomic write, rollback, rotation, deletion/reset |
| Enrollment | 3–8 bounds, duplicate rejection, one-frame actions, 120-second lifecycle, cancellation, no partial commit, failed replacement preserves the previous valid profile |
| Verification | match/no-match/ambiguous thresholds, median aggregation, no-profile/mismatch errors, cancellation, timeout, stale generations, rate limiting |
| KCM/QML | complete Home/Setup/Test/Diagnostics surface creates offscreen, 320/480/960 geometry, keyboard focus and accessibility names, destructive dialogs, page/app/preview teardown |
| Synthetic lifecycle | fake-worker camera discovery, preview, one-frame analysis, five-sample enrollment, atomic save, profile refresh, one-frame verification, clear, delete, and teardown repeated for 100 bounded cycles |
| Status/privacy | separate local identity capabilities, PAM/system states unsupported, aggregate-only support reports, no embeddings/scores in QML |
| Security | no PAM/authselect writes, service, privilege, socket/network, runtime download, production fake selector, arbitrary production vault root |
| Packaging | exact dependencies/files/licenses/workers, ordinary permissions, no auth scriptlets, reproducible archive/SRPM/RPM and isolated lifecycle |
| Package transition | v3 payload fixture is obsoleted by v4, old KCM files removed, user/PAM sentinels preserved, clean install/reinstall/remove deterministic |
| Release workflow | least privilege, exactly one source archive/binary RPM/SRPM, complete strict checksums, seven-day temporary retention, fail-closed published-release upload |
| Localization | all active user-visible messages have checked Swedish translations |

Hardware, representative FAR/FRR, demographic/bias behavior, liveness, and
spoof resistance are deliberately separate qualification evidence. The
synthetic lifecycle checks state and worker cleanup only; it is not hardware or
authentication qualification.
