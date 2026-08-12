// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::env;
use std::fmt;
use std::fmt::Write as FmtWrite;
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, Read, Write};
#[cfg(test)]
use std::os::unix::fs::PermissionsExt;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

use kfaceauth_crypto_openssl_sys::{
    CryptoError, KEY_BYTES, NONCE_BYTES, TAG_BYTES, current_uid, decrypt, encrypt, random,
};
use kfaceauth_identity_types::{
    DETECTOR_MODEL_ID, EMBEDDING_DIMENSION, EMBEDDING_FORMAT_ID, EMBEDDING_MODEL_ID,
    NORMALIZATION_VERSION, NormalizedEmbedding, SFACE_MODEL_SHA256, cosine_similarity,
};

pub const MINIMUM_PROFILE_SAMPLES: usize = 3;
pub const RECOMMENDED_PROFILE_SAMPLES: usize = 5;
pub const MAXIMUM_PROFILE_SAMPLES: usize = 8;
pub const MASTER_KEY_BYTES: usize = KEY_BYTES;
pub const PROVISIONAL_MATCH_THRESHOLD: f64 = 0.45;
pub const PROVISIONAL_AMBIGUITY_MARGIN: f64 = 0.04;

const PRODUCT_DIRECTORY: &str = "kfaceauth";
const VAULT_FILE: &str = "identity.vault";
const LOCK_FILE: &str = "identity.lock";
const VAULT_MAGIC: &[u8; 8] = b"KFAVLT04";
const PROFILE_MAGIC: &[u8; 8] = b"KFAIDP01";
const VAULT_SCHEMA: u16 = 1;
const PROFILE_SCHEMA: u16 = 1;
const OUTER_HEADER_BYTES: usize = 8 + 2 + NONCE_BYTES + 4 + TAG_BYTES;
const MAXIMUM_VAULT_BYTES: usize = 16 * 1024;
const LOCK_TIMEOUT: Duration = Duration::from_secs(2);
const LOCK_RETRY: Duration = Duration::from_millis(10);

pub struct MasterKey {
    bytes: [u8; KEY_BYTES],
}

impl MasterKey {
    /// Generates an AES-256 master key using the Fedora OpenSSL CSPRNG.
    ///
    /// # Errors
    ///
    /// Returns a stable provider error if the system CSPRNG fails.
    pub fn generate() -> Result<Self, VaultError> {
        Ok(Self {
            bytes: random::<KEY_BYTES>()?,
        })
    }

    #[must_use]
    pub const fn from_bytes(bytes: [u8; KEY_BYTES]) -> Self {
        Self { bytes }
    }

    /// Returns key bytes only for a private anonymous-channel transfer or an
    /// injected test provider. Never log, persist, or place them in argv/env.
    #[must_use]
    pub const fn sensitive_bytes(&self) -> &[u8; KEY_BYTES] {
        &self.bytes
    }
}

impl Drop for MasterKey {
    fn drop(&mut self) {
        self.bytes.fill(0);
    }
}

pub trait KeyProvider {
    /// Retrieves the current user-session key.
    ///
    /// # Errors
    ///
    /// Locked, cancelled, and unavailable providers must remain distinct.
    fn master_key(&mut self) -> Result<MasterKey, KeyProviderError>;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum KeyProviderError {
    Locked,
    Cancelled,
    Unavailable,
}

impl fmt::Display for KeyProviderError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Locked => "user-session key provider is locked",
            Self::Cancelled => "user cancelled key access",
            Self::Unavailable => "user-session key provider is unavailable",
        })
    }
}

impl std::error::Error for KeyProviderError {}

pub struct Profile {
    samples: Vec<NormalizedEmbedding>,
}

impl Profile {
    /// Constructs a bounded single-user profile.
    ///
    /// # Errors
    ///
    /// Profiles outside the 3..=8 sample contract are rejected.
    pub fn new(samples: Vec<NormalizedEmbedding>) -> Result<Self, VaultError> {
        if !(MINIMUM_PROFILE_SAMPLES..=MAXIMUM_PROFILE_SAMPLES).contains(&samples.len()) {
            return Err(VaultError::ProfileBounds);
        }
        Ok(Self { samples })
    }

    #[must_use]
    pub fn sample_count(&self) -> usize {
        self.samples.len()
    }

    /// Applies the centrally owned experimental median aggregation policy.
    ///
    /// No score escapes this backend method.
    #[must_use]
    pub fn verify(&self, candidate: &NormalizedEmbedding) -> VerificationResult {
        let mut scores: Vec<f64> = self
            .samples
            .iter()
            .map(|sample| cosine_similarity(sample, candidate))
            .collect();
        scores.sort_by(f64::total_cmp);
        let median = if scores.len() % 2 == 0 {
            let upper = scores.len() / 2;
            f64::midpoint(scores[upper - 1], scores[upper])
        } else {
            scores[scores.len() / 2]
        };
        if median >= PROVISIONAL_MATCH_THRESHOLD {
            VerificationResult::Match
        } else if median >= PROVISIONAL_MATCH_THRESHOLD - PROVISIONAL_AMBIGUITY_MARGIN {
            VerificationResult::Ambiguous
        } else {
            VerificationResult::NoMatch
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum VerificationResult {
    Match,
    NoMatch,
    Ambiguous,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProfileSummary {
    pub enrolled: bool,
    pub sample_count: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum VaultStatus {
    Absent,
    Ready(ProfileSummary),
    Corrupt,
    ModelMismatch,
    Unavailable,
}

pub struct Vault {
    root: PathBuf,
    uid: u32,
}

impl Vault {
    /// Constructs the production vault at the fixed XDG user-data location.
    ///
    /// Production callers cannot inject an arbitrary state root or UID.
    ///
    /// # Errors
    ///
    /// Returns an unavailable error if no safe absolute XDG/home location can
    /// be derived.
    pub fn production() -> Result<Self, VaultError> {
        let base = match env::var_os("XDG_DATA_HOME").map(PathBuf::from) {
            Some(path) if path.is_absolute() => path,
            Some(_) => return Err(VaultError::Unavailable),
            None => {
                let home = env::var_os("HOME")
                    .map(PathBuf::from)
                    .filter(|path| path.is_absolute())
                    .ok_or(VaultError::Unavailable)?;
                home.join(".local/share")
            }
        };
        Ok(Self {
            root: base.join(PRODUCT_DIRECTORY),
            uid: current_uid(),
        })
    }

    #[cfg(test)]
    fn for_test(root: PathBuf, uid: u32) -> Self {
        Self { root, uid }
    }

    /// Returns only aggregate, non-sensitive profile state.
    #[must_use]
    pub fn status(&self, key: Option<&MasterKey>) -> VaultStatus {
        if !self.root.exists() {
            return VaultStatus::Absent;
        }
        if validate_directory(&self.root, self.uid).is_err() {
            return VaultStatus::Unavailable;
        }
        if !self.root.join(VAULT_FILE).exists() {
            return VaultStatus::Absent;
        }
        let Some(key) = key else {
            return VaultStatus::Unavailable;
        };
        match self.open_profile(key) {
            Ok(profile) => match u8::try_from(profile.sample_count()) {
                Ok(sample_count) => VaultStatus::Ready(ProfileSummary {
                    enrolled: true,
                    sample_count,
                }),
                Err(_) => VaultStatus::Unavailable,
            },
            Err(VaultError::NotFound) => VaultStatus::Absent,
            Err(VaultError::ModelMismatch) => VaultStatus::ModelMismatch,
            Err(
                VaultError::Crypto(CryptoError::AuthenticationFailure)
                | VaultError::Corrupt
                | VaultError::UnknownSchema,
            ) => VaultStatus::Corrupt,
            Err(_) => VaultStatus::Unavailable,
        }
    }

    /// Authenticates, decrypts, and validates the complete profile.
    ///
    /// # Errors
    ///
    /// Filesystem, AEAD, schema, UID, model, and embedding failures are
    /// fail-closed and preserve the original file.
    pub fn open_profile(&self, key: &MasterKey) -> Result<Profile, VaultError> {
        validate_directory(&self.root, self.uid)?;
        let lock = VaultLock::acquire(&self.root, self.uid)?;
        let bytes = SensitiveBytes(read_secure_file(&self.root.join(VAULT_FILE), self.uid)?);
        let profile = decode_vault(&bytes.0, key, self.uid);
        drop(lock);
        profile
    }

    /// Atomically commits an all-or-nothing profile.
    ///
    /// Existing unreadable data is never overwritten.
    ///
    /// # Errors
    ///
    /// Returns without replacing the original vault if validation, encryption,
    /// temporary verification, fsync, or rename fails.
    pub fn commit_profile(&self, key: &MasterKey, profile: &Profile) -> Result<(), VaultError> {
        ensure_directory(&self.root, self.uid)?;
        let _lock = VaultLock::acquire(&self.root, self.uid)?;
        let final_path = self.root.join(VAULT_FILE);
        if final_path.exists() {
            let existing = SensitiveBytes(read_secure_file(&final_path, self.uid)?);
            drop(decode_vault(&existing.0, key, self.uid)?);
        }
        let encoded = SensitiveBytes(encode_vault(profile, key, self.uid)?);
        write_verified_atomic(&self.root, &final_path, &encoded.0, key, self.uid)
    }

    /// Re-encrypts a validated profile under a new key using an atomic replace.
    ///
    /// # Errors
    ///
    /// The old file remains untouched on any failure.
    pub fn rotate_key(&self, old_key: &MasterKey, new_key: &MasterKey) -> Result<(), VaultError> {
        validate_directory(&self.root, self.uid)?;
        let _lock = VaultLock::acquire(&self.root, self.uid)?;
        let final_path = self.root.join(VAULT_FILE);
        let old_bytes = SensitiveBytes(read_secure_file(&final_path, self.uid)?);
        let profile = decode_vault(&old_bytes.0, old_key, self.uid)?;
        let new_bytes = SensitiveBytes(encode_vault(&profile, new_key, self.uid)?);
        write_verified_atomic(&self.root, &final_path, &new_bytes.0, new_key, self.uid)
    }

    /// Deletes a valid profile after authenticating it with the current key.
    ///
    /// This does not claim guaranteed physical erasure on SSD, `CoW`, snapshots,
    /// backups, or journaling filesystems.
    ///
    /// # Errors
    ///
    /// Rejects absent, unauthenticated, corrupt, or filesystem-unsafe data.
    pub fn delete_profile(&self, key: &MasterKey) -> Result<(), VaultError> {
        validate_directory(&self.root, self.uid)?;
        let _lock = VaultLock::acquire(&self.root, self.uid)?;
        let path = self.root.join(VAULT_FILE);
        let bytes = SensitiveBytes(read_secure_file(&path, self.uid)?);
        drop(decode_vault(&bytes.0, key, self.uid)?);
        fs::remove_file(path)?;
        sync_directory(&self.root)
    }

    /// Explicitly deletes unreadable profile data after validating filesystem
    /// ownership/type/mode. The caller must provide destructive confirmation.
    ///
    /// # Errors
    ///
    /// Rejects absent or filesystem-unsafe data and preserves it on failure.
    pub fn reset_unreadable(&self) -> Result<(), VaultError> {
        validate_directory(&self.root, self.uid)?;
        let _lock = VaultLock::acquire(&self.root, self.uid)?;
        let path = self.root.join(VAULT_FILE);
        validate_secure_path(&path, self.uid)?;
        fs::remove_file(path)?;
        sync_directory(&self.root)
    }

    /// Verifies vault integrity without returning profile material.
    ///
    /// # Errors
    ///
    /// Returns the same fail-closed validation errors as [`Self::open_profile`].
    pub fn validate_integrity(&self, key: &MasterKey) -> Result<ProfileSummary, VaultError> {
        let profile = self.open_profile(key)?;
        let sample_count =
            u8::try_from(profile.sample_count()).map_err(|_| VaultError::ProfileBounds)?;
        Ok(ProfileSummary {
            enrolled: true,
            sample_count,
        })
    }
}

#[derive(Debug)]
pub enum VaultError {
    Io(io::Error),
    Crypto(CryptoError),
    KeyProvider(KeyProviderError),
    Unavailable,
    NotFound,
    UnsafeFilesystem,
    LockTimeout,
    Oversized,
    Corrupt,
    UnknownSchema,
    WrongUid,
    ModelMismatch,
    ProfileBounds,
    InvalidEmbedding,
}

impl fmt::Display for VaultError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Io(_) => "vault I/O failed",
            Self::Crypto(_) => "vault cryptographic validation failed",
            Self::KeyProvider(_) => "vault key provider is unavailable",
            Self::Unavailable => "vault location is unavailable",
            Self::NotFound => "face profile is absent",
            Self::UnsafeFilesystem => "vault filesystem metadata is unsafe",
            Self::LockTimeout => "vault lock acquisition timed out",
            Self::Oversized => "vault exceeds its byte bound",
            Self::Corrupt => "vault data is corrupt",
            Self::UnknownSchema => "vault schema is unsupported",
            Self::WrongUid => "vault is bound to another numeric UID",
            Self::ModelMismatch => "vault model identity does not match the runtime",
            Self::ProfileBounds => "profile sample count is outside the allowed bounds",
            Self::InvalidEmbedding => "vault contains an invalid embedding",
        })
    }
}

impl std::error::Error for VaultError {}

impl From<io::Error> for VaultError {
    fn from(error: io::Error) -> Self {
        if error.kind() == io::ErrorKind::NotFound {
            Self::NotFound
        } else {
            Self::Io(error)
        }
    }
}

impl From<CryptoError> for VaultError {
    fn from(error: CryptoError) -> Self {
        Self::Crypto(error)
    }
}

impl From<KeyProviderError> for VaultError {
    fn from(error: KeyProviderError) -> Self {
        Self::KeyProvider(error)
    }
}

struct SensitiveBytes(Vec<u8>);

impl Drop for SensitiveBytes {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}

fn associated_data(uid: u32) -> Vec<u8> {
    let mut data = Vec::with_capacity(256);
    data.extend_from_slice(b"io.github.loofiboss_bit.KFaceAuth/user-session-vault");
    data.extend_from_slice(&VAULT_SCHEMA.to_be_bytes());
    data.extend_from_slice(&uid.to_be_bytes());
    data.extend_from_slice(DETECTOR_MODEL_ID.as_bytes());
    data.push(0);
    data.extend_from_slice(EMBEDDING_MODEL_ID.as_bytes());
    data.push(0);
    data.extend_from_slice(SFACE_MODEL_SHA256.as_bytes());
    data.push(0);
    data.extend_from_slice(EMBEDDING_FORMAT_ID.as_bytes());
    data.extend_from_slice(
        &u16::try_from(EMBEDDING_DIMENSION)
            .expect("embedding dimension fits u16")
            .to_be_bytes(),
    );
    data.extend_from_slice(&NORMALIZATION_VERSION.to_be_bytes());
    data
}

fn encode_profile(profile: &Profile, uid: u32) -> Result<SensitiveBytes, VaultError> {
    let mut plaintext = SensitiveBytes(Vec::with_capacity(
        128 + profile.sample_count() * EMBEDDING_DIMENSION * 4,
    ));
    plaintext.0.extend_from_slice(PROFILE_MAGIC);
    plaintext.0.extend_from_slice(&PROFILE_SCHEMA.to_be_bytes());
    plaintext.0.extend_from_slice(&uid.to_be_bytes());
    push_string(&mut plaintext.0, DETECTOR_MODEL_ID)?;
    push_string(&mut plaintext.0, EMBEDDING_MODEL_ID)?;
    push_string(&mut plaintext.0, SFACE_MODEL_SHA256)?;
    push_string(&mut plaintext.0, EMBEDDING_FORMAT_ID)?;
    plaintext.0.extend_from_slice(
        &u16::try_from(EMBEDDING_DIMENSION)
            .expect("embedding dimension fits u16")
            .to_be_bytes(),
    );
    plaintext
        .0
        .extend_from_slice(&NORMALIZATION_VERSION.to_be_bytes());
    plaintext
        .0
        .push(u8::try_from(profile.sample_count()).expect("profile sample count is bounded"));
    for embedding in &profile.samples {
        for value in embedding.sensitive_values() {
            plaintext.0.extend_from_slice(&value.to_le_bytes());
        }
    }
    Ok(plaintext)
}

fn decode_profile(plaintext: &[u8], expected_uid: u32) -> Result<Profile, VaultError> {
    let mut cursor = Cursor::new(plaintext);
    if cursor.take(8)? != PROFILE_MAGIC {
        return Err(VaultError::Corrupt);
    }
    let schema = cursor.u16()?;
    if schema != PROFILE_SCHEMA {
        return Err(VaultError::UnknownSchema);
    }
    if cursor.u32()? != expected_uid {
        return Err(VaultError::WrongUid);
    }
    if cursor.string()? != DETECTOR_MODEL_ID
        || cursor.string()? != EMBEDDING_MODEL_ID
        || cursor.string()? != SFACE_MODEL_SHA256
        || cursor.string()? != EMBEDDING_FORMAT_ID
        || usize::from(cursor.u16()?) != EMBEDDING_DIMENSION
        || cursor.u16()? != NORMALIZATION_VERSION
    {
        return Err(VaultError::ModelMismatch);
    }
    let sample_count = usize::from(cursor.u8()?);
    if !(MINIMUM_PROFILE_SAMPLES..=MAXIMUM_PROFILE_SAMPLES).contains(&sample_count) {
        return Err(VaultError::ProfileBounds);
    }
    let expected_bytes = sample_count
        .checked_mul(EMBEDDING_DIMENSION)
        .and_then(|count| count.checked_mul(4))
        .ok_or(VaultError::Corrupt)?;
    if cursor.remaining() != expected_bytes {
        return Err(VaultError::Corrupt);
    }
    let mut samples = Vec::with_capacity(sample_count);
    for _ in 0..sample_count {
        let mut values = [0.0_f32; EMBEDDING_DIMENSION];
        for value in &mut values {
            *value = f32::from_le_bytes(
                cursor
                    .take(4)?
                    .try_into()
                    .map_err(|_| VaultError::Corrupt)?,
            );
        }
        samples.push(
            NormalizedEmbedding::from_normalized(values)
                .map_err(|_| VaultError::InvalidEmbedding)?,
        );
    }
    Profile::new(samples)
}

fn encode_vault(profile: &Profile, key: &MasterKey, uid: u32) -> Result<Vec<u8>, VaultError> {
    let plaintext = encode_profile(profile, uid)?;
    let nonce = random::<NONCE_BYTES>()?;
    let aad = SensitiveBytes(associated_data(uid));
    let (ciphertext, tag) = encrypt(key.sensitive_bytes(), &nonce, &aad.0, &plaintext.0)?;
    let mut output = Vec::with_capacity(OUTER_HEADER_BYTES + ciphertext.len());
    output.extend_from_slice(VAULT_MAGIC);
    output.extend_from_slice(&VAULT_SCHEMA.to_be_bytes());
    output.extend_from_slice(&nonce);
    output.extend_from_slice(
        &u32::try_from(ciphertext.len())
            .map_err(|_| VaultError::Oversized)?
            .to_be_bytes(),
    );
    output.extend_from_slice(&tag);
    output.extend_from_slice(&ciphertext);
    if output.len() > MAXIMUM_VAULT_BYTES {
        output.fill(0);
        return Err(VaultError::Oversized);
    }
    Ok(output)
}

fn decode_vault(bytes: &[u8], key: &MasterKey, uid: u32) -> Result<Profile, VaultError> {
    if bytes.len() < OUTER_HEADER_BYTES || bytes.len() > MAXIMUM_VAULT_BYTES {
        return Err(VaultError::Corrupt);
    }
    if &bytes[..8] != VAULT_MAGIC {
        return Err(VaultError::Corrupt);
    }
    let schema = u16::from_be_bytes(bytes[8..10].try_into().map_err(|_| VaultError::Corrupt)?);
    if schema != VAULT_SCHEMA {
        return Err(VaultError::UnknownSchema);
    }
    let nonce: &[u8; NONCE_BYTES] = bytes[10..10 + NONCE_BYTES]
        .try_into()
        .map_err(|_| VaultError::Corrupt)?;
    let length_offset = 10 + NONCE_BYTES;
    let ciphertext_length = usize::try_from(u32::from_be_bytes(
        bytes[length_offset..length_offset + 4]
            .try_into()
            .map_err(|_| VaultError::Corrupt)?,
    ))
    .map_err(|_| VaultError::Corrupt)?;
    let tag_offset = length_offset + 4;
    let tag: &[u8; TAG_BYTES] = bytes[tag_offset..tag_offset + TAG_BYTES]
        .try_into()
        .map_err(|_| VaultError::Corrupt)?;
    let ciphertext_offset = tag_offset + TAG_BYTES;
    if ciphertext_length == 0
        || ciphertext_offset.checked_add(ciphertext_length) != Some(bytes.len())
    {
        return Err(VaultError::Corrupt);
    }
    let aad = SensitiveBytes(associated_data(uid));
    let plaintext = SensitiveBytes(decrypt(
        key.sensitive_bytes(),
        nonce,
        &aad.0,
        &bytes[ciphertext_offset..],
        tag,
    )?);
    decode_profile(&plaintext.0, uid)
}

fn push_string(output: &mut Vec<u8>, value: &str) -> Result<(), VaultError> {
    output.push(u8::try_from(value.len()).map_err(|_| VaultError::Corrupt)?);
    output.extend_from_slice(value.as_bytes());
    Ok(())
}

struct Cursor<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Cursor<'a> {
    const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn take(&mut self, count: usize) -> Result<&'a [u8], VaultError> {
        let end = self.offset.checked_add(count).ok_or(VaultError::Corrupt)?;
        let value = self
            .bytes
            .get(self.offset..end)
            .ok_or(VaultError::Corrupt)?;
        self.offset = end;
        Ok(value)
    }

    fn u8(&mut self) -> Result<u8, VaultError> {
        Ok(*self.take(1)?.first().ok_or(VaultError::Corrupt)?)
    }

    fn u16(&mut self) -> Result<u16, VaultError> {
        Ok(u16::from_be_bytes(
            self.take(2)?.try_into().map_err(|_| VaultError::Corrupt)?,
        ))
    }

    fn u32(&mut self) -> Result<u32, VaultError> {
        Ok(u32::from_be_bytes(
            self.take(4)?.try_into().map_err(|_| VaultError::Corrupt)?,
        ))
    }

    fn string(&mut self) -> Result<&'a str, VaultError> {
        let length = usize::from(self.u8()?);
        std::str::from_utf8(self.take(length)?).map_err(|_| VaultError::Corrupt)
    }

    const fn remaining(&self) -> usize {
        self.bytes.len() - self.offset
    }
}

fn ensure_directory(path: &Path, uid: u32) -> Result<(), VaultError> {
    if !path.exists() {
        let mut builder = DirBuilder::new();
        builder.recursive(true).mode(0o700);
        builder.create(path)?;
    }
    validate_directory(path, uid)
}

fn validate_directory(path: &Path, uid: u32) -> Result<(), VaultError> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.file_type().is_dir()
        || metadata.file_type().is_symlink()
        || metadata.uid() != uid
        || metadata.mode() & 0o777 != 0o700
    {
        return Err(VaultError::UnsafeFilesystem);
    }
    Ok(())
}

fn validate_secure_path(path: &Path, uid: u32) -> Result<fs::Metadata, VaultError> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.file_type().is_file()
        || metadata.file_type().is_symlink()
        || metadata.uid() != uid
        || metadata.mode() & 0o777 != 0o600
        || metadata.nlink() != 1
    {
        return Err(VaultError::UnsafeFilesystem);
    }
    Ok(metadata)
}

fn read_secure_file(path: &Path, uid: u32) -> Result<Vec<u8>, VaultError> {
    let before = validate_secure_path(path, uid)?;
    if usize::try_from(before.len()).map_or(true, |size| size > MAXIMUM_VAULT_BYTES) {
        return Err(VaultError::Oversized);
    }
    let file = OpenOptions::new().read(true).open(path)?;
    let after = file.metadata()?;
    if before.dev() != after.dev()
        || before.ino() != after.ino()
        || after.uid() != uid
        || after.mode() & 0o777 != 0o600
        || after.nlink() != 1
    {
        return Err(VaultError::UnsafeFilesystem);
    }
    let mut bytes =
        Vec::with_capacity(usize::try_from(after.len()).map_err(|_| VaultError::Oversized)?);
    file.take(
        u64::try_from(MAXIMUM_VAULT_BYTES)
            .expect("maximum vault bytes fit u64")
            .saturating_add(1),
    )
    .read_to_end(&mut bytes)?;
    if bytes.len() > MAXIMUM_VAULT_BYTES {
        bytes.fill(0);
        return Err(VaultError::Oversized);
    }
    Ok(bytes)
}

struct VaultLock {
    path: PathBuf,
}

impl VaultLock {
    fn acquire(root: &Path, uid: u32) -> Result<Self, VaultError> {
        validate_directory(root, uid)?;
        let path = root.join(LOCK_FILE);
        let deadline = Instant::now()
            .checked_add(LOCK_TIMEOUT)
            .ok_or(VaultError::LockTimeout)?;
        loop {
            match OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(&path)
            {
                Ok(file) => {
                    let metadata = file.metadata()?;
                    if metadata.uid() != uid
                        || metadata.mode() & 0o777 != 0o600
                        || metadata.nlink() != 1
                    {
                        drop(file);
                        let _ = fs::remove_file(&path);
                        return Err(VaultError::UnsafeFilesystem);
                    }
                    file.sync_all()?;
                    return Ok(Self { path });
                }
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                    if Instant::now() >= deadline {
                        return Err(VaultError::LockTimeout);
                    }
                    thread::sleep(LOCK_RETRY);
                }
                Err(error) => return Err(VaultError::Io(error)),
            }
        }
    }
}

impl Drop for VaultLock {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

fn write_verified_atomic(
    root: &Path,
    final_path: &Path,
    bytes: &[u8],
    key: &MasterKey,
    uid: u32,
) -> Result<(), VaultError> {
    if bytes.is_empty() || bytes.len() > MAXIMUM_VAULT_BYTES {
        return Err(VaultError::Oversized);
    }
    let suffix = random::<8>()?;
    let suffix = hex_suffix(&suffix);
    let temporary_path = root.join(format!(".identity.vault.{suffix}.tmp"));
    let mut cleanup = TemporaryFile {
        path: temporary_path.clone(),
        armed: true,
    };
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&temporary_path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    drop(file);
    let verified = SensitiveBytes(read_secure_file(&temporary_path, uid)?);
    drop(decode_vault(&verified.0, key, uid)?);
    fs::rename(&temporary_path, final_path)?;
    cleanup.armed = false;
    sync_directory(root)
}

struct TemporaryFile {
    path: PathBuf,
    armed: bool,
}

impl Drop for TemporaryFile {
    fn drop(&mut self) {
        if self.armed {
            let _ = fs::remove_file(&self.path);
        }
    }
}

fn sync_directory(path: &Path) -> Result<(), VaultError> {
    File::open(path)?.sync_all()?;
    Ok(())
}

fn hex_suffix(bytes: &[u8]) -> String {
    bytes.iter().fold(
        String::with_capacity(bytes.len() * 2),
        |mut output, value| {
            let _ = write!(&mut output, "{value:02x}");
            output
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temporary_root(name: &str) -> PathBuf {
        let nonce = random::<8>().unwrap();
        let suffix = hex_suffix(&nonce);
        env::temp_dir().join(format!("kfaceauth-{name}-{suffix}"))
    }

    fn embedding(axis: usize) -> NormalizedEmbedding {
        let mut values = [0.0_f32; EMBEDDING_DIMENSION];
        values[axis] = 1.0;
        NormalizedEmbedding::from_normalized(values).unwrap()
    }

    fn profile() -> Profile {
        Profile::new(vec![embedding(0), embedding(0), embedding(0)]).unwrap()
    }

    #[test]
    fn aead_round_trip_status_and_delete() {
        let root = temporary_root("roundtrip");
        let vault = Vault::for_test(root.clone(), current_uid());
        let key = MasterKey::generate().unwrap();
        vault.commit_profile(&key, &profile()).unwrap();
        assert_eq!(
            vault.status(Some(&key)),
            VaultStatus::Ready(ProfileSummary {
                enrolled: true,
                sample_count: 3
            })
        );
        assert_eq!(vault.open_profile(&key).unwrap().sample_count(), 3);
        vault.delete_profile(&key).unwrap();
        assert_eq!(vault.status(Some(&key)), VaultStatus::Absent);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn wrong_key_ciphertext_and_aad_modification_fail_closed() {
        let root = temporary_root("tamper");
        let vault = Vault::for_test(root.clone(), current_uid());
        let key = MasterKey::generate().unwrap();
        vault.commit_profile(&key, &profile()).unwrap();
        let wrong = MasterKey::generate().unwrap();
        assert!(matches!(
            vault.open_profile(&wrong),
            Err(VaultError::Crypto(CryptoError::AuthenticationFailure))
        ));

        let path = root.join(VAULT_FILE);
        let mut bytes = fs::read(&path).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 1;
        fs::write(&path, &bytes).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        assert!(matches!(
            vault.open_profile(&key),
            Err(VaultError::Crypto(CryptoError::AuthenticationFailure))
        ));
        assert_eq!(fs::read(&path).unwrap(), bytes);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn nonce_uniqueness_and_key_rotation() {
        let root = temporary_root("rotate");
        let vault = Vault::for_test(root.clone(), current_uid());
        let old = MasterKey::generate().unwrap();
        vault.commit_profile(&old, &profile()).unwrap();
        let first = fs::read(root.join(VAULT_FILE)).unwrap();
        let new = MasterKey::generate().unwrap();
        vault.rotate_key(&old, &new).unwrap();
        let second = fs::read(root.join(VAULT_FILE)).unwrap();
        assert_ne!(&first[10..10 + NONCE_BYTES], &second[10..10 + NONCE_BYTES]);
        assert!(vault.open_profile(&old).is_err());
        assert_eq!(vault.open_profile(&new).unwrap().sample_count(), 3);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn filesystem_metadata_and_bounds_are_rejected() {
        let root = temporary_root("metadata");
        let vault = Vault::for_test(root.clone(), current_uid());
        let key = MasterKey::generate().unwrap();
        vault.commit_profile(&key, &profile()).unwrap();
        let path = root.join(VAULT_FILE);

        fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();
        assert!(matches!(
            vault.open_profile(&key),
            Err(VaultError::UnsafeFilesystem)
        ));
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();

        let link = root.join("hardlink");
        fs::hard_link(&path, &link).unwrap();
        assert!(matches!(
            vault.open_profile(&key),
            Err(VaultError::UnsafeFilesystem)
        ));
        fs::remove_file(link).unwrap();
        fs::remove_file(&path).unwrap();
        std::os::unix::fs::symlink("/dev/null", &path).unwrap();
        assert!(matches!(
            vault.reset_unreadable(),
            Err(VaultError::UnsafeFilesystem)
        ));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn profile_limits_and_threshold_policy_are_closed() {
        assert!(matches!(
            Profile::new(vec![embedding(0), embedding(0)]),
            Err(VaultError::ProfileBounds)
        ));
        let profile = profile();
        assert_eq!(profile.verify(&embedding(0)), VerificationResult::Match);
        assert_eq!(profile.verify(&embedding(1)), VerificationResult::NoMatch);

        let mut ambiguous = [0.0_f32; EMBEDDING_DIMENSION];
        ambiguous[0] = 0.43;
        ambiguous[1] = (1.0_f32 - 0.43_f32.powi(2)).sqrt();
        let ambiguous = NormalizedEmbedding::from_normalized(ambiguous).unwrap();
        assert_eq!(profile.verify(&ambiguous), VerificationResult::Ambiguous);
    }

    #[test]
    fn plaintext_embeddings_and_key_are_absent_from_vault() {
        let root = temporary_root("plaintext");
        let vault = Vault::for_test(root.clone(), current_uid());
        let key = MasterKey::from_bytes([0xA5; KEY_BYTES]);
        vault.commit_profile(&key, &profile()).unwrap();
        let bytes = fs::read(root.join(VAULT_FILE)).unwrap();
        assert!(
            !bytes
                .windows(KEY_BYTES)
                .any(|window| window == [0xA5; KEY_BYTES])
        );
        let embedding_bytes = 1.0_f32.to_le_bytes();
        assert!(
            !bytes
                .windows(embedding_bytes.len())
                .any(|window| window == embedding_bytes)
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn truncated_oversized_and_unknown_outer_schema_are_rejected() {
        let root = temporary_root("outer-format");
        let vault = Vault::for_test(root.clone(), current_uid());
        let key = MasterKey::generate().unwrap();
        vault.commit_profile(&key, &profile()).unwrap();
        let path = root.join(VAULT_FILE);
        let original = fs::read(&path).unwrap();

        fs::write(&path, &original[..OUTER_HEADER_BYTES - 1]).unwrap();
        assert!(matches!(vault.open_profile(&key), Err(VaultError::Corrupt)));

        fs::write(&path, vec![0; MAXIMUM_VAULT_BYTES + 1]).unwrap();
        assert!(matches!(
            vault.open_profile(&key),
            Err(VaultError::Oversized)
        ));

        let mut unknown = original;
        unknown[8..10].copy_from_slice(&2_u16.to_be_bytes());
        fs::write(&path, &unknown).unwrap();
        assert!(matches!(
            vault.open_profile(&key),
            Err(VaultError::UnknownSchema)
        ));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn inner_uid_schema_model_dimension_and_normalization_are_bound() {
        let uid = current_uid();
        let encoded = encode_profile(&profile(), uid).unwrap();
        assert!(matches!(
            decode_profile(&encoded.0, uid.wrapping_add(1)),
            Err(VaultError::WrongUid)
        ));

        let mut unknown_schema = encoded.0.clone();
        unknown_schema[8..10].copy_from_slice(&2_u16.to_be_bytes());
        assert!(matches!(
            decode_profile(&unknown_schema, uid),
            Err(VaultError::UnknownSchema)
        ));

        let detector_offset = 8 + 2 + 4 + 1;
        let mut wrong_model = encoded.0.clone();
        wrong_model[detector_offset] ^= 1;
        assert!(matches!(
            decode_profile(&wrong_model, uid),
            Err(VaultError::ModelMismatch)
        ));

        let metadata_bytes = 8
            + 2
            + 4
            + 1
            + DETECTOR_MODEL_ID.len()
            + 1
            + EMBEDDING_MODEL_ID.len()
            + 1
            + SFACE_MODEL_SHA256.len()
            + 1
            + EMBEDDING_FORMAT_ID.len();
        let mut wrong_dimension = encoded.0.clone();
        wrong_dimension[metadata_bytes..metadata_bytes + 2].copy_from_slice(&127_u16.to_be_bytes());
        assert!(matches!(
            decode_profile(&wrong_dimension, uid),
            Err(VaultError::ModelMismatch)
        ));

        let mut wrong_normalization = encoded.0.clone();
        wrong_normalization[metadata_bytes + 2..metadata_bytes + 4]
            .copy_from_slice(&2_u16.to_be_bytes());
        assert!(matches!(
            decode_profile(&wrong_normalization, uid),
            Err(VaultError::ModelMismatch)
        ));
    }

    #[test]
    fn lock_is_bounded_and_atomic_failure_preserves_original() {
        let root = temporary_root("lock-atomic");
        let vault = Vault::for_test(root.clone(), current_uid());
        let key = MasterKey::generate().unwrap();
        vault.commit_profile(&key, &profile()).unwrap();
        let final_path = root.join(VAULT_FILE);
        let original = fs::read(&final_path).unwrap();

        {
            let _lock = VaultLock::acquire(&root, current_uid()).unwrap();
            assert!(matches!(
                VaultLock::acquire(&root, current_uid()),
                Err(VaultError::LockTimeout)
            ));
        }

        assert!(
            write_verified_atomic(
                &root,
                &final_path,
                &[0; OUTER_HEADER_BYTES],
                &key,
                current_uid()
            )
            .is_err()
        );
        assert_eq!(fs::read(&final_path).unwrap(), original);
        assert!(
            fs::read_dir(&root)
                .unwrap()
                .flatten()
                .all(|entry| !entry.file_name().to_string_lossy().ends_with(".tmp"))
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn failed_rotation_preserves_the_old_profile() {
        let root = temporary_root("rotation-rollback");
        let vault = Vault::for_test(root.clone(), current_uid());
        let old = MasterKey::generate().unwrap();
        vault.commit_profile(&old, &profile()).unwrap();
        let original = fs::read(root.join(VAULT_FILE)).unwrap();
        let wrong_old = MasterKey::generate().unwrap();
        let replacement = MasterKey::generate().unwrap();

        assert!(vault.rotate_key(&wrong_old, &replacement).is_err());
        assert_eq!(fs::read(root.join(VAULT_FILE)).unwrap(), original);
        assert_eq!(vault.open_profile(&old).unwrap().sample_count(), 3);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn key_provider_states_do_not_fall_back() {
        struct UnavailableProvider(KeyProviderError);

        impl KeyProvider for UnavailableProvider {
            fn master_key(&mut self) -> Result<MasterKey, KeyProviderError> {
                Err(self.0)
            }
        }

        for state in [
            KeyProviderError::Locked,
            KeyProviderError::Cancelled,
            KeyProviderError::Unavailable,
        ] {
            let mut provider = UnavailableProvider(state);
            assert_eq!(provider.master_key().err(), Some(state));
        }
    }
}
