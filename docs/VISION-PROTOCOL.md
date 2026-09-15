# Vision worker protocol v1

The KCM starts `/usr/libexec/kfaceauth-vision-worker` directly, without a
shell, for one explicitly requested analysis. The worker is unprivileged and
communicates only through stdin and stdout. It processes one frame, emits one
response, clears its state, and exits.

Every message is a four-byte unsigned big-endian payload length followed by the
payload. Multi-byte integer fields are unsigned big-endian. There are no
strings, paths, free-form operations, identities, embeddings, or
authentication decisions on the wire.

## Analyze request

| Offset | Size | Field | Bound |
|---:|---:|---|---|
| 0 | 2 | protocol version | exactly `1` |
| 2 | 1 | operation | exactly `1` (`analyze`) |
| 3 | 1 | pixel format | `1` RGB8, `2` RGBA8, `3` Gray8 |
| 4 | 8 | generation | positive and chosen by the parent |
| 12 | 4 | timeout, milliseconds | `1..5000` |
| 16 | 2 | width | `1..1920` |
| 18 | 2 | height | `1..1080` |
| 20 | 4 | stride | at least packed row bytes, bounded by payload |
| 24 | remaining | pixel bytes | exactly `stride * height` |

All size, channel, row, stride, and payload calculations are checked before
allocation or indexing. The largest permitted request payload is
`8,294,424` bytes (`24 + 1920 * 1080 * 4`). Truncated, oversized, trailing,
overflowing, or unsupported input is rejected.

## Success response

| Offset | Size | Field | Bound |
|---:|---:|---|---|
| 0 | 2 | protocol version | exactly `1` |
| 2 | 1 | response kind | exactly `0x81` |
| 3 | 1 | face count | `0..8` |
| 4 | 8 | generation | must equal the active request |
| 12 | 1 | brightness quality | bounded `0..255` |
| 13 | 1 | contrast quality | bounded `0..255` |
| 14 | 1 | sharpness quality | bounded `0..255` |
| 15 | 1 | quality flags | defined bit set only |
| 16 | 8 each | face rectangles | `x, y, width, height` as `u16` |

Every rectangle must be non-empty and contained by the request dimensions.
YuNet returns five landmarks and a detector score internally. Production
validates them against the original frame, then discards both before this
response is encoded.

`face_count` is interpreted only as zero, one, or multiple faces. Quality
values provide neutral camera guidance. Neither is identity, liveness,
anti-spoofing, or authentication evidence.

## Error response

An error response is exactly 12 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | protocol version `1` |
| 2 | 1 | response kind `0xff` |
| 3 | 1 | stable error code |
| 4 | 8 | request generation, or zero if it could not be decoded |

Stable errors distinguish invalid framing, unsupported version/operation/pixel
format, invalid dimensions/stride/payload, cancellation, timeout, provider
unavailability, invalid native detector output, and internal failure.
Model-manifest, artifact-integrity, and provider initialization failures expose
only stable error `11` after the request generation has been decoded. Invalid
runtime output exposes stable error `12`. Error messages never contain model bytes,
pixels, embeddings, paths supplied by the request, or biometric output.

## Parent lifecycle

The parent enforces separate startup, inference, and shutdown timers. It accepts
only one current generation, bounds stdout and stderr, rejects any extra or
malformed response, and kills the worker after a protocol violation. A result
is discarded if its generation is stale.

Analysis is cancelled and the copied frame/result is cleared when preview
stops, the Setup page is hidden, the application deactivates, the KCM is
destroyed, or a newer analysis supersedes it. No frame is exposed to QML or
written to a temporary file.
