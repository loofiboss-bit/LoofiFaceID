// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::fmt;
use std::io::{self, Read, Write};
use std::path::Path;
use std::time::Duration;

use kfaceauth_templates::{
    MASTER_KEY_BYTES as KEY_BYTES, MAXIMUM_PROFILE_SAMPLES, MasterKey, Profile, Vault, VaultError,
    VaultStatus, VerificationResult,
};
use kfaceauth_vision::identity::{
    EMBEDDING_DIMENSION, IdentityError, IdentityLoadError, IdentityProvider, NormalizedEmbedding,
    cosine_similarity,
};
use kfaceauth_vision::{
    CancellationToken, ImageError, ImageView, MAX_FRAME_BYTES, PixelFormat, ProcessingControl,
};

pub const IDENTITY_PROTOCOL_VERSION: u16 = 1;
pub const MAX_IDENTITY_REQUEST_BYTES: usize =
    64 + MAX_FRAME_BYTES + MAXIMUM_PROFILE_SAMPLES * EMBEDDING_DIMENSION * 4;
pub const MAX_IDENTITY_RESPONSE_BYTES: usize = 64 + EMBEDDING_DIMENSION * 4;
pub const MIN_TIMEOUT_MS: u32 = 1;
pub const MAX_TIMEOUT_MS: u32 = 10_000;

pub const OP_STATUS: u8 = 1;
pub const OP_EXTRACT_ENROLLMENT_SAMPLE: u8 = 2;
pub const OP_COMMIT_ENROLLMENT: u8 = 3;
pub const OP_LIST_PROFILE_SUMMARY: u8 = 4;
pub const OP_VERIFY_ONE_FRAME: u8 = 5;
pub const OP_DELETE_PROFILE: u8 = 6;
pub const OP_ROTATE_VAULT_KEY: u8 = 7;
pub const OP_VALIDATE_VAULT: u8 = 8;
pub const OP_RESET_UNREADABLE: u8 = 9;
pub const OP_GENERATE_KEY: u8 = 10;

pub const RESPONSE_STATUS: u8 = 0x81;
pub const RESPONSE_SAMPLE: u8 = 0x82;
pub const RESPONSE_ACK: u8 = 0x83;
pub const RESPONSE_VERIFICATION: u8 = 0x84;
pub const RESPONSE_KEY: u8 = 0x85;
pub const RESPONSE_ERROR: u8 = 0xff;

const COMMON_HEADER_BYTES: usize = 16;
const IMAGE_HEADER_BYTES: usize = 8;
const DUPLICATE_COSINE_THRESHOLD: f64 = 0.995;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum WorkerErrorCode {
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    UnsupportedOperation = 3,
    InvalidGeneration = 4,
    InvalidTimeout = 5,
    InvalidFrame = 6,
    NoFace = 7,
    MultipleFaces = 8,
    PoorQuality = 9,
    FaceGeometry = 10,
    DuplicateSample = 11,
    ProfileBounds = 12,
    NoProfile = 13,
    VaultLocked = 14,
    VaultCorrupt = 15,
    ModelMismatch = 16,
    KeyUnavailable = 17,
    Cancelled = 18,
    DeadlineExceeded = 19,
    ModelUnavailable = 20,
    RateLimited = 21,
    InternalFailure = 22,
}

#[derive(Debug)]
pub enum IdentityWorkerError {
    Io(io::Error),
    EmptyFrame,
    FrameTooLarge,
    TrailingData,
}

impl fmt::Display for IdentityWorkerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(_) => formatter.write_str("identity worker protocol I/O failed"),
            Self::EmptyFrame => formatter.write_str("identity worker frame is empty"),
            Self::FrameTooLarge => formatter.write_str("identity worker frame is oversized"),
            Self::TrailingData => formatter.write_str("identity worker received trailing data"),
        }
    }
}

impl std::error::Error for IdentityWorkerError {}

impl From<io::Error> for IdentityWorkerError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

enum Request<'a> {
    Status(Option<MasterKey>),
    Extract {
        prior: Vec<NormalizedEmbedding>,
        image: ImageView<'a>,
    },
    Commit {
        key: MasterKey,
        samples: Vec<NormalizedEmbedding>,
    },
    Summary(MasterKey),
    Verify {
        key: MasterKey,
        image: ImageView<'a>,
    },
    Delete(MasterKey),
    Rotate {
        old_key: MasterKey,
        new_key: MasterKey,
    },
    Validate(MasterKey),
    ResetUnreadable,
    GenerateKey,
}

struct ParsedRequest<'a> {
    generation: u64,
    timeout_ms: u32,
    request: Request<'a>,
}

struct RequestError {
    code: WorkerErrorCode,
    generation: u64,
}

/// Serves exactly one bounded request and clears request/response buffers.
///
/// # Errors
///
/// Serves exactly one bounded request and clears request/response buffers.
///
/// # Errors
///
/// Framing and local pipe I/O errors terminate the worker without retry.
pub fn serve_once<R: Read, W: Write>(
    reader: &mut R,
    writer: &mut W,
    model_root: &Path,
) -> Result<(), IdentityWorkerError> {
    let mut payload = SensitiveBytes(read_frame(reader)?);
    require_eof(reader)?;
    let mut response = SensitiveBytes(match parse_request(&payload.0) {
        Ok(request) => process_request(request, model_root),
        Err(error) => encode_error(error.code, error.generation),
    });
    payload.0.fill(0);
    let result = write_frame(writer, &response.0);
    response.0.fill(0);
    result
}

/// Serves a continuous session of identity worker requests until the input reaches EOF.
///
/// Models and verified runtime graphs are cached across requests in the session.
///
/// # Errors
///
/// Framing and protocol I/O errors return [`IdentityWorkerError`].
pub fn serve_session<R: Read, W: Write>(
    reader: &mut R,
    writer: &mut W,
    model_root: &Path,
) -> Result<(), IdentityWorkerError> {
    let mut provider_cache: Option<IdentityProvider> = None;
    loop {
        let mut header = [0_u8; 4];
        match reader.read_exact(&mut header) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(error) => return Err(IdentityWorkerError::Io(error)),
        }
        let length = u32::from_be_bytes(header) as usize;
        if length == 0 {
            return Err(IdentityWorkerError::EmptyFrame);
        }
        if length > MAX_IDENTITY_REQUEST_BYTES {
            return Err(IdentityWorkerError::FrameTooLarge);
        }
        let mut payload = SensitiveBytes(vec![0_u8; length]);
        if let Err(error) = reader.read_exact(&mut payload.0) {
            payload.0.fill(0);
            return Err(IdentityWorkerError::Io(error));
        }
        let mut response = SensitiveBytes(match parse_request(&payload.0) {
            Ok(request) => process_request_with_cache(request, model_root, &mut provider_cache),
            Err(error) => encode_error(error.code, error.generation),
        });
        payload.0.fill(0);
        let result = write_frame(writer, &response.0);
        response.0.fill(0);
        result?;
    }
}

fn get_provider<'a>(
    cache: &'a mut Option<IdentityProvider>,
    model_root: &Path,
) -> Result<&'a IdentityProvider, IdentityLoadError> {
    if cache.is_none() {
        *cache = Some(IdentityProvider::from_model_root(model_root)?);
    }
    Ok(cache.as_ref().expect("provider was populated"))
}

fn process_request(request: ParsedRequest<'_>, model_root: &Path) -> Vec<u8> {
    let mut cache = None;
    process_request_with_cache(request, model_root, &mut cache)
}

#[allow(clippy::too_many_lines)]
fn process_request_with_cache(
    request: ParsedRequest<'_>,
    model_root: &Path,
    provider_cache: &mut Option<IdentityProvider>,
) -> Vec<u8> {
    let generation = request.generation;
    let cancellation = CancellationToken::default();
    let Ok(control) = ProcessingControl::with_timeout(
        &cancellation,
        Duration::from_millis(u64::from(request.timeout_ms)),
    ) else {
        return encode_error(WorkerErrorCode::DeadlineExceeded, generation);
    };

    match request.request {
        Request::Status(key) => {
            if let Err(error) = get_provider(provider_cache, model_root) {
                return encode_error(map_load_error(&error), generation);
            }
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            encode_status(generation, vault.status(key.as_ref()))
        }
        Request::Extract { prior, image } => {
            let provider = match get_provider(provider_cache, model_root) {
                Ok(provider) => provider,
                Err(error) => return encode_error(map_load_error(&error), generation),
            };
            match provider.extract(image, control) {
                Ok(sample)
                    if prior.iter().any(|existing| {
                        cosine_similarity(existing, &sample) >= DUPLICATE_COSINE_THRESHOLD
                    }) =>
                {
                    encode_error(WorkerErrorCode::DuplicateSample, generation)
                }
                Ok(sample) => encode_sample(generation, &sample),
                Err(error) => encode_error(map_identity_error(&error), generation),
            }
        }
        Request::Commit { key, samples } => {
            let profile = match Profile::new(samples) {
                Ok(profile) => profile,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            match vault.commit_profile(&key, &profile) {
                Ok(()) => encode_ack(generation),
                Err(error) => encode_error(map_vault_error(&error), generation),
            }
        }
        Request::Summary(key) => {
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            encode_status(generation, vault.status(Some(&key)))
        }
        Request::Verify { key, image } => {
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            let profile = match vault.open_profile(&key) {
                Ok(profile) => profile,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            let provider = match get_provider(provider_cache, model_root) {
                Ok(provider) => provider,
                Err(error) => return encode_error(map_load_error(&error), generation),
            };
            match provider.extract(image, control) {
                Ok(candidate) => encode_verification(generation, profile.verify(&candidate)),
                Err(error) => encode_error(map_identity_error(&error), generation),
            }
        }
        Request::Delete(key) => {
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            match vault.delete_profile(&key) {
                Ok(()) => encode_ack(generation),
                Err(error) => encode_error(map_vault_error(&error), generation),
            }
        }
        Request::Rotate { old_key, new_key } => {
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            match vault.rotate_key(&old_key, &new_key) {
                Ok(()) => encode_ack(generation),
                Err(error) => encode_error(map_vault_error(&error), generation),
            }
        }
        Request::Validate(key) => {
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            match vault.validate_integrity(&key) {
                Ok(summary) => encode_status(generation, VaultStatus::Ready(summary)),
                Err(error) => encode_error(map_vault_error(&error), generation),
            }
        }
        Request::ResetUnreadable => {
            let vault = match Vault::production() {
                Ok(vault) => vault,
                Err(error) => return encode_error(map_vault_error(&error), generation),
            };
            match vault.reset_unreadable() {
                Ok(()) => encode_ack(generation),
                Err(error) => encode_error(map_vault_error(&error), generation),
            }
        }
        Request::GenerateKey => match MasterKey::generate() {
            Ok(key) => encode_key(generation, &key),
            Err(error) => encode_error(map_vault_error(&error), generation),
        },
    }
}

#[allow(clippy::too_many_lines)]
fn parse_request(payload: &[u8]) -> Result<ParsedRequest<'_>, RequestError> {
    let generation = payload
        .get(4..12)
        .and_then(|value| value.try_into().ok())
        .map_or(0, u64::from_be_bytes);
    if payload.len() < COMMON_HEADER_BYTES {
        return Err(RequestError {
            code: WorkerErrorCode::InvalidRequest,
            generation,
        });
    }
    if u16::from_be_bytes(payload[0..2].try_into().expect("checked header"))
        != IDENTITY_PROTOCOL_VERSION
    {
        return Err(RequestError {
            code: WorkerErrorCode::UnsupportedVersion,
            generation,
        });
    }
    if generation == 0 {
        return Err(RequestError {
            code: WorkerErrorCode::InvalidGeneration,
            generation,
        });
    }
    let operation = payload[2];
    let flags = payload[3];
    let timeout_ms = u32::from_be_bytes(payload[12..16].try_into().expect("checked header"));
    if !(MIN_TIMEOUT_MS..=MAX_TIMEOUT_MS).contains(&timeout_ms) {
        return Err(RequestError {
            code: WorkerErrorCode::InvalidTimeout,
            generation,
        });
    }
    let data = &payload[COMMON_HEADER_BYTES..];
    let invalid = |code| RequestError { code, generation };
    let request = match operation {
        OP_STATUS if flags <= 1 => {
            if flags == 0 && data.is_empty() {
                Request::Status(None)
            } else if flags == 1 && data.len() == KEY_BYTES {
                Request::Status(Some(MasterKey::from_bytes(
                    data.try_into()
                        .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
                )))
            } else {
                return Err(invalid(WorkerErrorCode::InvalidRequest));
            }
        }
        OP_EXTRACT_ENROLLMENT_SAMPLE if flags == 0 => {
            let (prior, image) = parse_enrollment_image(data).map_err(invalid)?;
            Request::Extract { prior, image }
        }
        OP_COMMIT_ENROLLMENT if flags == 0 => {
            if data.len() < KEY_BYTES + 1 {
                return Err(invalid(WorkerErrorCode::InvalidRequest));
            }
            let key = MasterKey::from_bytes(
                data[..KEY_BYTES]
                    .try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            );
            let samples = parse_embeddings(&data[KEY_BYTES..]).map_err(invalid)?;
            Request::Commit { key, samples }
        }
        OP_LIST_PROFILE_SUMMARY if flags == 0 && data.len() == KEY_BYTES => {
            Request::Summary(MasterKey::from_bytes(
                data.try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            ))
        }
        OP_VERIFY_ONE_FRAME if flags == 0 && data.len() > KEY_BYTES => {
            let key = MasterKey::from_bytes(
                data[..KEY_BYTES]
                    .try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            );
            let image = parse_image(&data[KEY_BYTES..]).map_err(invalid)?;
            Request::Verify { key, image }
        }
        OP_DELETE_PROFILE if flags == 0 && data.len() == KEY_BYTES => {
            Request::Delete(MasterKey::from_bytes(
                data.try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            ))
        }
        OP_ROTATE_VAULT_KEY if flags == 0 && data.len() == KEY_BYTES * 2 => Request::Rotate {
            old_key: MasterKey::from_bytes(
                data[..KEY_BYTES]
                    .try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            ),
            new_key: MasterKey::from_bytes(
                data[KEY_BYTES..]
                    .try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            ),
        },
        OP_VALIDATE_VAULT if flags == 0 && data.len() == KEY_BYTES => {
            Request::Validate(MasterKey::from_bytes(
                data.try_into()
                    .map_err(|_| invalid(WorkerErrorCode::InvalidRequest))?,
            ))
        }
        OP_RESET_UNREADABLE if flags == 0 && data.is_empty() => Request::ResetUnreadable,
        OP_GENERATE_KEY if flags == 0 && data.is_empty() => Request::GenerateKey,
        OP_STATUS
        | OP_EXTRACT_ENROLLMENT_SAMPLE
        | OP_COMMIT_ENROLLMENT
        | OP_LIST_PROFILE_SUMMARY
        | OP_VERIFY_ONE_FRAME
        | OP_DELETE_PROFILE
        | OP_ROTATE_VAULT_KEY
        | OP_VALIDATE_VAULT
        | OP_RESET_UNREADABLE
        | OP_GENERATE_KEY => return Err(invalid(WorkerErrorCode::InvalidRequest)),
        _ => return Err(invalid(WorkerErrorCode::UnsupportedOperation)),
    };
    Ok(ParsedRequest {
        generation,
        timeout_ms,
        request,
    })
}

fn parse_enrollment_image(
    data: &[u8],
) -> Result<(Vec<NormalizedEmbedding>, ImageView<'_>), WorkerErrorCode> {
    let count = usize::from(*data.first().ok_or(WorkerErrorCode::InvalidRequest)?);
    if count >= MAXIMUM_PROFILE_SAMPLES {
        return Err(WorkerErrorCode::ProfileBounds);
    }
    let embedding_bytes = count
        .checked_mul(EMBEDDING_DIMENSION)
        .and_then(|value| value.checked_mul(4))
        .ok_or(WorkerErrorCode::InvalidRequest)?;
    let image_offset = 1_usize
        .checked_add(embedding_bytes)
        .ok_or(WorkerErrorCode::InvalidRequest)?;
    if data.len() <= image_offset {
        return Err(WorkerErrorCode::InvalidRequest);
    }
    let mut encoded = Vec::with_capacity(1 + embedding_bytes);
    encoded.push(u8::try_from(count).expect("sample count is bounded"));
    encoded.extend_from_slice(&data[1..image_offset]);
    let prior = parse_embeddings(&encoded)?;
    Ok((prior, parse_image(&data[image_offset..])?))
}

fn parse_embeddings(data: &[u8]) -> Result<Vec<NormalizedEmbedding>, WorkerErrorCode> {
    let count = usize::from(*data.first().ok_or(WorkerErrorCode::InvalidRequest)?);
    if count > MAXIMUM_PROFILE_SAMPLES {
        return Err(WorkerErrorCode::ProfileBounds);
    }
    let expected = 1_usize
        .checked_add(
            count
                .checked_mul(EMBEDDING_DIMENSION)
                .and_then(|value| value.checked_mul(4))
                .ok_or(WorkerErrorCode::InvalidRequest)?,
        )
        .ok_or(WorkerErrorCode::InvalidRequest)?;
    if data.len() != expected {
        return Err(WorkerErrorCode::InvalidRequest);
    }
    let mut samples = Vec::with_capacity(count);
    let mut offset = 1;
    for _ in 0..count {
        let mut values = [0.0_f32; EMBEDDING_DIMENSION];
        for value in &mut values {
            *value = f32::from_le_bytes(
                data[offset..offset + 4]
                    .try_into()
                    .map_err(|_| WorkerErrorCode::InvalidRequest)?,
            );
            offset += 4;
        }
        samples.push(
            NormalizedEmbedding::from_normalized(values)
                .map_err(|_| WorkerErrorCode::InvalidRequest)?,
        );
    }
    Ok(samples)
}

fn parse_image(data: &[u8]) -> Result<ImageView<'_>, WorkerErrorCode> {
    if data.len() < IMAGE_HEADER_BYTES {
        return Err(WorkerErrorCode::InvalidFrame);
    }
    let format = PixelFormat::try_from(data[0]).map_err(map_image_error)?;
    if data[1] != 0 {
        return Err(WorkerErrorCode::InvalidFrame);
    }
    let width = u32::from(u16::from_be_bytes(
        data[2..4]
            .try_into()
            .map_err(|_| WorkerErrorCode::InvalidFrame)?,
    ));
    let height = u32::from(u16::from_be_bytes(
        data[4..6]
            .try_into()
            .map_err(|_| WorkerErrorCode::InvalidFrame)?,
    ));
    let stride = u32::from(u16::from_be_bytes(
        data[6..8]
            .try_into()
            .map_err(|_| WorkerErrorCode::InvalidFrame)?,
    ));
    ImageView::new(format, width, height, stride, &data[IMAGE_HEADER_BYTES..])
        .map_err(map_image_error)
}

fn encode_status(generation: u64, status: VaultStatus) -> Vec<u8> {
    let (state, sample_count) = match status {
        VaultStatus::Absent => (0, 0),
        VaultStatus::Ready(summary) => (1, summary.sample_count),
        VaultStatus::Corrupt => (2, 0),
        VaultStatus::ModelMismatch => (3, 0),
        VaultStatus::Unavailable => (4, 0),
    };
    let mut response = response_header(RESPONSE_STATUS, state, generation);
    response.push(sample_count);
    response
}

fn encode_sample(generation: u64, embedding: &NormalizedEmbedding) -> Vec<u8> {
    let mut response = response_header(RESPONSE_SAMPLE, 0, generation);
    for value in embedding.sensitive_values() {
        response.extend_from_slice(&value.to_le_bytes());
    }
    response
}

fn encode_ack(generation: u64) -> Vec<u8> {
    response_header(RESPONSE_ACK, 0, generation)
}

fn encode_verification(generation: u64, result: VerificationResult) -> Vec<u8> {
    let code = match result {
        VerificationResult::Match => 1,
        VerificationResult::NoMatch => 2,
        VerificationResult::Ambiguous => 3,
    };
    response_header(RESPONSE_VERIFICATION, code, generation)
}

fn encode_key(generation: u64, key: &MasterKey) -> Vec<u8> {
    let mut response = response_header(RESPONSE_KEY, 0, generation);
    response.extend_from_slice(key.sensitive_bytes());
    response
}

fn encode_error(code: WorkerErrorCode, generation: u64) -> Vec<u8> {
    response_header(RESPONSE_ERROR, code as u8, generation)
}

fn response_header(kind: u8, code: u8, generation: u64) -> Vec<u8> {
    let mut response = Vec::with_capacity(12);
    response.extend_from_slice(&IDENTITY_PROTOCOL_VERSION.to_be_bytes());
    response.push(kind);
    response.push(code);
    response.extend_from_slice(&generation.to_be_bytes());
    response
}

fn read_frame<R: Read>(reader: &mut R) -> Result<Vec<u8>, IdentityWorkerError> {
    let mut length = [0_u8; 4];
    reader.read_exact(&mut length)?;
    let length = usize::try_from(u32::from_be_bytes(length))
        .map_err(|_| IdentityWorkerError::FrameTooLarge)?;
    if length == 0 {
        return Err(IdentityWorkerError::EmptyFrame);
    }
    if length > MAX_IDENTITY_REQUEST_BYTES {
        return Err(IdentityWorkerError::FrameTooLarge);
    }
    let mut payload = vec![0_u8; length];
    if let Err(error) = reader.read_exact(&mut payload) {
        payload.fill(0);
        return Err(IdentityWorkerError::Io(error));
    }
    Ok(payload)
}

fn require_eof<R: Read>(reader: &mut R) -> Result<(), IdentityWorkerError> {
    let mut trailing = [0_u8; 1];
    match reader.read(&mut trailing) {
        Ok(0) => Ok(()),
        Ok(_) => Err(IdentityWorkerError::TrailingData),
        Err(error) => Err(IdentityWorkerError::Io(error)),
    }
}

fn write_frame<W: Write>(writer: &mut W, payload: &[u8]) -> Result<(), IdentityWorkerError> {
    if payload.is_empty() || payload.len() > MAX_IDENTITY_RESPONSE_BYTES {
        return Err(IdentityWorkerError::FrameTooLarge);
    }
    writer.write_all(
        &u32::try_from(payload.len())
            .map_err(|_| IdentityWorkerError::FrameTooLarge)?
            .to_be_bytes(),
    )?;
    writer.write_all(payload)?;
    writer.flush()?;
    Ok(())
}

fn map_image_error(_error: ImageError) -> WorkerErrorCode {
    WorkerErrorCode::InvalidFrame
}

fn map_identity_error(error: &IdentityError) -> WorkerErrorCode {
    match error {
        IdentityError::Cancelled => WorkerErrorCode::Cancelled,
        IdentityError::DeadlineExceeded => WorkerErrorCode::DeadlineExceeded,
        IdentityError::NoFace => WorkerErrorCode::NoFace,
        IdentityError::MultipleFaces => WorkerErrorCode::MultipleFaces,
        IdentityError::PoorQuality => WorkerErrorCode::PoorQuality,
        IdentityError::FaceGeometry => WorkerErrorCode::FaceGeometry,
        IdentityError::InvalidEmbedding | IdentityError::Runtime(_) => {
            WorkerErrorCode::InternalFailure
        }
    }
}

fn map_load_error(_error: &IdentityLoadError) -> WorkerErrorCode {
    WorkerErrorCode::ModelUnavailable
}

fn map_vault_error(error: &VaultError) -> WorkerErrorCode {
    match error {
        VaultError::NotFound => WorkerErrorCode::NoProfile,
        VaultError::Crypto(_)
        | VaultError::Corrupt
        | VaultError::UnknownSchema
        | VaultError::WrongUid
        | VaultError::InvalidEmbedding => WorkerErrorCode::VaultCorrupt,
        VaultError::ModelMismatch => WorkerErrorCode::ModelMismatch,
        VaultError::ProfileBounds => WorkerErrorCode::ProfileBounds,
        VaultError::KeyProvider(_) => WorkerErrorCode::KeyUnavailable,
        VaultError::Io(_)
        | VaultError::Unavailable
        | VaultError::UnsafeFilesystem
        | VaultError::LockTimeout
        | VaultError::Oversized => WorkerErrorCode::InternalFailure,
    }
}

struct SensitiveBytes(Vec<u8>);

impl Drop for SensitiveBytes {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn framed(payload: &[u8]) -> Vec<u8> {
        let mut output = Vec::new();
        output.extend_from_slice(&u32::try_from(payload.len()).unwrap().to_be_bytes());
        output.extend_from_slice(payload);
        output
    }

    fn header(operation: u8, generation: u64) -> Vec<u8> {
        let mut payload = Vec::new();
        payload.extend_from_slice(&IDENTITY_PROTOCOL_VERSION.to_be_bytes());
        payload.push(operation);
        payload.push(0);
        payload.extend_from_slice(&generation.to_be_bytes());
        payload.extend_from_slice(&1_000_u32.to_be_bytes());
        payload
    }

    #[test]
    fn protocol_bounds_and_positive_generation_are_enforced() {
        let error = parse_request(&header(OP_GENERATE_KEY, 0)).err().unwrap();
        assert_eq!(error.code, WorkerErrorCode::InvalidGeneration);
        assert!(matches!(
            read_frame(&mut Cursor::new(framed(&vec![
                0;
                MAX_IDENTITY_REQUEST_BYTES
                    + 1
            ]))),
            Err(IdentityWorkerError::FrameTooLarge)
        ));
    }

    #[test]
    fn generated_key_response_is_private_and_exactly_bounded() {
        let response = process_request(
            ParsedRequest {
                generation: 7,
                timeout_ms: 1_000,
                request: Request::GenerateKey,
            },
            Path::new("/unread"),
        );
        assert_eq!(response.len(), 12 + KEY_BYTES);
        assert_eq!(response[2], RESPONSE_KEY);
        assert_eq!(u64::from_be_bytes(response[4..12].try_into().unwrap()), 7);
    }

    #[test]
    fn malformed_embeddings_never_reach_profile_code() {
        let mut payload = header(OP_COMMIT_ENROLLMENT, 9);
        payload.extend_from_slice(&[1; KEY_BYTES]);
        payload.push(3);
        payload.extend_from_slice(&vec![0; 3 * EMBEDDING_DIMENSION * 4]);
        let error = parse_request(&payload).err().unwrap();
        assert_eq!(error.code, WorkerErrorCode::InvalidRequest);
    }

    #[test]
    fn response_never_contains_similarity_scores() {
        for result in [
            VerificationResult::Match,
            VerificationResult::NoMatch,
            VerificationResult::Ambiguous,
        ] {
            assert_eq!(encode_verification(3, result).len(), 12);
        }
    }
}
