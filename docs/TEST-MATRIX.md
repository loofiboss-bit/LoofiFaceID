# Test matrix

> The current product is an experimental Fedora 44/KDE local
> profile/comparison utility. Rows describing PAM, PAD, authentication,
> accelerator, performance, or physical qualification are historical targets
> until independently reproduced; they are not current capability evidence.

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
| M5 Liveness & PAD (experimental primitives only) | Internal threshold logic and fail-closed texture-analysis errors; no physical presentation studies, no qualified anti-spoofing system, no ISO/IEC claim |
| M5 Gate Verification | Not run. Prior synthetic gate counts and qualification claims were withdrawn; see `RELEASE-QUALIFICATION-V5.1.md` |

Hardware, representative FAR/FRR, demographic/bias behavior, liveness, and
spoof resistance are deliberately separate qualification evidence. The
synthetic lifecycle checks state and worker cleanup only; it is not hardware or
authentication qualification.
