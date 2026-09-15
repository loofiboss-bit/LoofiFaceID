# Camera preview protocol v1

The KCM starts `/usr/libexec/kfaceauth-camera-preview-worker` directly,
without a shell. Commands travel on stdin and responses on stdout. Each record
is a four-byte unsigned big-endian length followed by one CBOR map. The JPEG
field is limited to 512 KiB; the parser also applies the raw-frame record bound
from `PreviewProtocol::MaxRecordBytes`.

Every record contains:

| Key | Type | Rule |
| --- | --- | --- |
| `protocol` | integer | exactly `1` |
| `session` | string | non-empty, at most 64 characters, fixed per worker |
| `sequence` | positive integer | strictly increasing in its direction |
| `type` | string | one of the fixed types below |

## Parent commands

- `discover`: no additional keys.
- `start`: one `device` string containing an opaque, in-memory worker token.
- `stop`: no additional keys.

Unknown keys, paths, free-form arguments, reused sequences, wrong sessions,
invalid CBOR, zero lengths, and oversized records produce `protocol-error`.

## Worker responses

- `devices`: at most 16 `{token, label, spectrum}` maps.
- `started`: `seconds` is exactly 60.
- `frame`: JPEG bytes, width, height, `rgb|ir|unknown`, and cumulative dropped
  frame count.
- `stopped`: bounded reason and whether capture had been active.
- `error`: a stable error code.

Labels are UTF-8 and limited to 128 bytes. Frames are at most 1920×1080 and
512 KiB JPEG. Capture is throttled to 30 fps. Only one pending frame is retained;
new frames replace an older pending frame and increment the drop counter.
Control responses take priority over a pending frame.

The protocol carries no filesystem path, raw device identifier, audio,
biometric result, profile operation, PAM operation, or authentication
decision. Nothing in the protocol is persisted.
