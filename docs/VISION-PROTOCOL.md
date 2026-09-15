# Vision worker protocol v2

The KCM starts `/usr/libexec/kfaceauth-vision-worker` directly, without a
shell, for bounded local analysis. The worker is unprivileged and communicates
only through stdin and stdout. In one-shot mode it processes one frame and
exits. In `--session` mode it keeps the verified model warm and accepts a
sequence of framed requests until EOF.

Every message is a four-byte unsigned big-endian payload length followed by the
payload. Multi-byte integer fields are unsigned big-endian. There are no
strings, paths, identities, embeddings, or authentication decisions on the
wire.

## Analyze request

| Offset | Size | Field | Bound |
|---:|---:|---|---|
| 0 | 2 | protocol version | exactly `2` |
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
| 0 | 2 | protocol version | exactly `2` |
| 2 | 1 | response kind | exactly `0x81` |
| 3 | 1 | face count | `0..8` |
| 4 | 8 | generation | must equal the active request |
| 12 | 1 | brightness quality | bounded `0..255` |
| 13 | 1 | contrast quality | bounded `0..255` |
| 14 | 1 | sharpness quality | bounded `0..255` |
| 15 | 1 | quality flags | defined bit set only |
| 16 | 8 each | face rectangle | `x, y, width, height` as `u16` |
| 24 | 20 each | five YuNet landmarks | five `(x, y)` pairs as `u16` |

Each face record is exactly 28 bytes. The landmark order is right eye, left
eye, nose, right mouth corner, and left mouth corner. Rust validates every
coordinate against the original frame and the parent validates the rectangle,
landmark count, payload length, and generation before use. No raw landmark or
detector score is exposed to QML; the backend derives typed guidance states and
pose values.

`face_count` is interpreted only as zero, one, or multiple faces. Quality
values provide neutral framing guidance. Neither is identity, liveness,
anti-spoofing, or authentication evidence.

## Error response

An error response is exactly 12 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | protocol version `2` |
| 2 | 1 | response kind `0xff` |
| 3 | 1 | stable error code |
| 4 | 8 | request generation, or zero if it could not be decoded |

Stable errors distinguish invalid framing, unsupported version/operation/pixel
format, invalid dimensions/stride/payload, cancellation, timeout, provider
unavailability, invalid native detector output, and internal failure.

## Session and lifecycle

The parent keeps at most one request active, sends the latest available frame,
and limits live guidance to four analyses per second. Startup, inference, and
shutdown timers remain strict; stdout and stderr are bounded. A response with
an old generation, invalid coordinates, an unexpected length, or extra bytes
is rejected and the worker is stopped.

The session is stopped and copied frame/result data is cleared when guidance is
stopped, the camera stops, Setup is hidden, the application becomes inactive,
the session times out, or the KCM is destroyed. No camera image is written to a
temporary file or persisted by the vision worker.

The backend exposes only typed guidance states (`NoFace`, `MultipleFaces`,
`TooFar`, `TooClose`, `OffCenter`, `PoorLighting`, `Blurred`, `WrongPose`, and
`Ready`) plus a coarse detected pose. Raw landmarks and detector confidence
values never cross into QML.
