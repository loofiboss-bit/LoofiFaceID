# Template vault

Milestone 4 stores one encrypted face profile for the currently logged-in
numeric Linux UID. It is a user-session vault, not a pre-login authentication
database.

## Key design

The production `KeyProvider` is KDE KWallet. Folder `KFaceAuth` contains one
binary entry, `user-session-vault-master-key-v1`, holding only a random
32-byte AES key. Embeddings are never stored in KWallet. Key creation uses the
Fedora OpenSSL 3 CSPRNG, and the key crosses to the short-lived worker only in a
private anonymous pipe. It never enters argv, environment variables, normal
files, logs, CLI output, or support reports.

If KWallet is locked, cancelled, disabled, or unavailable, the operation
returns a stable unavailable state. Opening KWallet has a 15-second upper
bound. There is no plaintext-file fallback. Tests use an isolated in-memory
key/provider boundary.

KWallet is unavailable before login and does not establish disk-theft, TPM,
pre-login, or system-authentication protection. A future PAM-capable key
provider requires a separate format migration and security review.

## Cryptography and format

The vault uses Fedora OpenSSL 3 AES-256-GCM with a fresh 96-bit CSPRNG nonce
for every encryption. The outer binary format has magic `KFAVLT04`, schema,
nonce, bounded ciphertext length, 128-bit tag, and ciphertext. Maximum file
size is 16 KiB.

Authenticated associated data binds:

- product namespace `io.github.loofiboss_bit.KFaceAuth/user-session-vault`;
- outer schema;
- current numeric UID;
- detector model ID;
- embedding model ID and exact SHA-256;
- embedding format, dimension, and normalization version.

The encrypted plaintext repeats a separate magic/schema, UID, all model and
format identities, sample count, and 128-value normalized embeddings. The
profile contains 3–8 samples. Any mismatch, unknown schema, invalid embedding,
wrong key, modified metadata/ciphertext/tag, or unexpected length fails closed.

## Filesystem transaction

Production derives the fixed location internally from absolute
`XDG_DATA_HOME`, or `$HOME/.local/share` when XDG is unset:
`kfaceauth/identity.vault`. It does not accept an arbitrary root or UID.

- directory must be owned by the current UID, regular directory, mode `0700`;
- vault and lock must be regular, current-UID files, mode `0600`, one link;
- symlinks, hard links, wrong ownership/mode, oversized data, and inode swaps
  are rejected;
- a same-directory exclusive lock has a two-second bounded acquisition;
- writes use a unique `O_EXCL` temporary file, `fsync`, decrypt/validate
  verification, atomic rename, and directory `fsync`;
- existing unreadable data is never overwritten automatically.

Key rotation is decrypt-validate-re-encrypt-verify-atomic-replace. Any failure
before rename preserves the old vault and key. Schema migration follows the
same transaction and rollback rule; no implicit migration is currently
defined.

For a new enrollment, the generated key remains only in memory until the user
selects **Finish and save**. The backend then stores the key in KWallet before
asking the worker to commit the encrypted vault; this order prevents a
committed profile from becoming unreadable because its key was never stored.
If the vault commit fails or is cancelled, the newly stored, still-unused key
is removed from KWallet. KWallet and the filesystem do not provide a shared
atomic transaction, so a failed KWallet rollback is reported explicitly and
requires reset; no face embedding or partial vault is retained in that state.

Deletion first validates/authenticates the profile, removes the file, and
syncs the directory, then removes the corresponding KWallet entry. Failure to
remove the key is reported rather than described as a complete reset.
Explicit reset may remove an unreadable but filesystem-safe file after
destructive confirmation. Neither operation claims physical erasure on SSD,
CoW, journal, snapshot, backup, or remanent storage. Key loss requires
explicit reset and re-enrollment; no backup/export or login-password recovery
exists.
