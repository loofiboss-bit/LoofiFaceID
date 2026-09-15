# Identity worker protocol

`kfaceauth-identity-worker` is an unprivileged, short-lived, one-request
process. Production invokes its fixed installed path and supplies only private
inherited stdin/stdout pipes. It has no socket, shell, network, telemetry,
camera access, arbitrary model root, or long-lived service.

## Framing and lifecycle

Each direction uses a four-byte big-endian payload length followed by exactly
one payload. EOF must follow the request. The worker disables core dumps before
reading biometric input, processes one request, writes one response, clears
sensitive buffers where practical, and exits.

- Protocol version: `1`
- Request common header: 16 bytes
- Maximum request payload:
  `64 + 1920*1080*4 + 8*128*4 = 8,298,560` bytes
- Maximum response payload: `64 + 128*4 = 576` bytes
- Generation: positive big-endian `u64`
- Timeout: big-endian `u32`, `1..=10,000` milliseconds
- Image maximum: 1920×1080, exact packed RGB8/RGBA8/Gray8 payload
- Key: exactly 32 bytes
- Embedding: exactly 128 little-endian FP32 values
- Profile samples: 3..=8; an enrollment transaction recommends 5

The parent also enforces finite startup, operation, and shutdown timers. It
allows only one active identity process and kills a replaced, cancelled, or
timed-out generation before starting another.

## Request header

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | version |
| 2 | 1 | operation |
| 3 | 1 | flags |
| 4 | 8 | generation |
| 12 | 4 | timeout milliseconds |
| 16 | bounded | operation body |

Closed operations:

| Code | Operation | Body |
|---:|---|---|
| 1 | status | optional 32-byte key, selected by flag 0/1 |
| 2 | extract enrollment sample | prior count, prior embeddings, one image |
| 3 | commit enrollment | key, sample count, embeddings |
| 4 | list profile summary | key |
| 5 | verify one frame | key, one image |
| 6 | delete profile | key |
| 7 | rotate vault key | old key, new key |
| 8 | validate vault | key |
| 9 | reset unreadable vault | empty |
| 10 | generate random key | empty; private backend use only |

Production KWallet key generation uses OpenSSL directly in the KCM backend;
operation 10 remains a bounded private protocol primitive and does not expose a
CLI surface.

## Responses

All responses begin with version, response kind, one public code byte, and the
request generation. Response kinds are status `0x81`, sample `0x82`, ack
`0x83`, verification `0x84`, key `0x85`, and error `0xff`.

Only sample and key responses contain sensitive bodies, each with an exact
fixed size. They remain inside the native backend. Verification codes are only
`Match`, `No match`, or `Ambiguous`; no score is encoded. Stable public errors
cover malformed requests, version/generation/timeout/frame violations,
face/quality failures, duplicate or bounded-profile failures, absent/locked/
corrupt/mismatched vault state, cancellation/deadline/model failures,
rate-limiting, and internal failure.

Malformed, oversized, trailing, stale-generation, or unexpected response data
is rejected without partial success. Error strings never contain key,
embedding, score, image, landmark, path, or user-provided data.
