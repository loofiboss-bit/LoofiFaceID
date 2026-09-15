// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::fmt;
use std::io::{self, Read, Write};

use zeroize::Zeroize;

pub const PROTOCOL_VERSION: u16 = 2;
pub const WORKER_PROTOCOL_VERSION: u16 = 1;
pub const MAX_REQUEST_BYTES: usize = 4 * 1024;
pub const MAX_RESPONSE_BYTES: usize = 16 * 1024;
pub const MAX_REQUEST_BYTES_WIRE: u32 = 4 * 1024;
pub const MAX_RESPONSE_BYTES_WIRE: u32 = 16 * 1024;
pub const OPERATION_CAPABILITIES: u16 = 1 << 0;
pub const OPERATION_STATUS: u16 = 1 << 1;

const REQUEST_CAPABILITIES: u8 = 1;
const REQUEST_STATUS: u8 = 2;
const RESPONSE_CAPABILITIES: u8 = 0x81;
const RESPONSE_STATUS: u8 = 0x82;
const RESPONSE_ERROR: u8 = 0xff;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Request {
    Capabilities,
    Status,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OperationSupport {
    Unsupported,
    Supported,
}

impl OperationSupport {
    const fn wire_value(self) -> u8 {
        match self {
            Self::Unsupported => 0,
            Self::Supported => 1,
        }
    }

    fn from_wire(value: u8) -> Result<Self, CodecError> {
        match value {
            0 => Ok(Self::Unsupported),
            1 => Ok(Self::Supported),
            _ => Err(CodecError::InvalidPayload),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DisabledCapability {
    Disabled,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EngineState {
    Ready,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Capabilities {
    pub protocol_min: u16,
    pub protocol_max: u16,
    pub max_request_bytes: u32,
    pub max_response_bytes: u32,
    pub operations: u16,
    pub detector_analysis: OperationSupport,
    pub embedding_extraction: OperationSupport,
    pub user_session_enrollment: OperationSupport,
    pub encrypted_user_session_persistence: OperationSupport,
    pub explicit_local_verification: OperationSupport,
    pub profile_deletion: OperationSupport,
    pub pam_authentication: OperationSupport,
    pub pam_configuration: OperationSupport,
    pub authselect: OperationSupport,
    pub sddm_lock_screen: OperationSupport,
    pub sudo_polkit: OperationSupport,
    pub liveness: OperationSupport,
    pub security_tiers: OperationSupport,
    pub privileged_services: OperationSupport,
    pub network_access: DisabledCapability,
    pub runtime_model_downloads: DisabledCapability,
}

impl Capabilities {
    #[must_use]
    pub const fn local_identity() -> Self {
        Self {
            protocol_min: PROTOCOL_VERSION,
            protocol_max: PROTOCOL_VERSION,
            max_request_bytes: MAX_REQUEST_BYTES_WIRE,
            max_response_bytes: MAX_RESPONSE_BYTES_WIRE,
            operations: OPERATION_CAPABILITIES | OPERATION_STATUS,
            detector_analysis: OperationSupport::Supported,
            embedding_extraction: OperationSupport::Supported,
            user_session_enrollment: OperationSupport::Supported,
            encrypted_user_session_persistence: OperationSupport::Supported,
            explicit_local_verification: OperationSupport::Supported,
            profile_deletion: OperationSupport::Supported,
            pam_authentication: OperationSupport::Unsupported,
            pam_configuration: OperationSupport::Unsupported,
            authselect: OperationSupport::Unsupported,
            sddm_lock_screen: OperationSupport::Unsupported,
            sudo_polkit: OperationSupport::Unsupported,
            liveness: OperationSupport::Unsupported,
            security_tiers: OperationSupport::Unsupported,
            privileged_services: OperationSupport::Unsupported,
            network_access: DisabledCapability::Disabled,
            runtime_model_downloads: DisabledCapability::Disabled,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Status {
    pub state: EngineState,
    pub detector_analysis: OperationSupport,
    pub embedding_extraction: OperationSupport,
    pub user_session_enrollment: OperationSupport,
    pub encrypted_user_session_persistence: OperationSupport,
    pub explicit_local_verification: OperationSupport,
    pub profile_deletion: OperationSupport,
    pub pam_authentication: OperationSupport,
    pub pam_configuration: OperationSupport,
    pub authselect: OperationSupport,
    pub sddm_lock_screen: OperationSupport,
    pub sudo_polkit: OperationSupport,
    pub liveness: OperationSupport,
    pub security_tiers: OperationSupport,
    pub privileged_services: OperationSupport,
}

impl Status {
    #[must_use]
    pub const fn local_identity() -> Self {
        Self {
            state: EngineState::Ready,
            detector_analysis: OperationSupport::Supported,
            embedding_extraction: OperationSupport::Supported,
            user_session_enrollment: OperationSupport::Supported,
            encrypted_user_session_persistence: OperationSupport::Supported,
            explicit_local_verification: OperationSupport::Supported,
            profile_deletion: OperationSupport::Supported,
            pam_authentication: OperationSupport::Unsupported,
            pam_configuration: OperationSupport::Unsupported,
            authselect: OperationSupport::Unsupported,
            sddm_lock_screen: OperationSupport::Unsupported,
            sudo_polkit: OperationSupport::Unsupported,
            liveness: OperationSupport::Unsupported,
            security_tiers: OperationSupport::Unsupported,
            privileged_services: OperationSupport::Unsupported,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorCode {
    InvalidRequest,
    UnsupportedVersion,
    UnsupportedOperation,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Response {
    Capabilities(Capabilities),
    Status(Status),
    Error(ErrorCode),
}

#[derive(Debug)]
pub enum CodecError {
    Io(io::Error),
    EmptyFrame,
    FrameTooLarge { length: usize, maximum: usize },
    InvalidPayload,
    UnsupportedVersion(u16),
}

impl fmt::Display for CodecError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(formatter, "local protocol I/O failed: {error}"),
            Self::EmptyFrame => formatter.write_str("local protocol frame is empty"),
            Self::FrameTooLarge { length, maximum } => {
                write!(
                    formatter,
                    "local protocol frame length {length} exceeds {maximum}"
                )
            }
            Self::InvalidPayload => formatter.write_str("local protocol payload is invalid"),
            Self::UnsupportedVersion(version) => {
                write!(formatter, "local protocol version {version} is unsupported")
            }
        }
    }
}

impl std::error::Error for CodecError {}

impl From<io::Error> for CodecError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

#[derive(Debug)]
pub enum FrameError {
    Io(io::Error),
    EmptyFrame,
    FrameTooLarge { length: usize, maximum: usize },
    TrailingData,
}

impl fmt::Display for FrameError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(formatter, "bounded frame I/O failed: {error}"),
            Self::EmptyFrame => formatter.write_str("bounded frame is empty"),
            Self::FrameTooLarge { length, maximum } => {
                write!(formatter, "bounded frame length {length} exceeds {maximum}")
            }
            Self::TrailingData => formatter.write_str("bounded stream contains trailing data"),
        }
    }
}

impl std::error::Error for FrameError {}

impl From<io::Error> for FrameError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

#[must_use]
pub fn encode_request(request: Request) -> Vec<u8> {
    let kind = match request {
        Request::Capabilities => REQUEST_CAPABILITIES,
        Request::Status => REQUEST_STATUS,
    };
    let version = PROTOCOL_VERSION.to_be_bytes();
    vec![version[0], version[1], kind, 0]
}

/// Decodes one bounded request payload.
///
/// # Errors
///
/// Returns [`CodecError`] when the payload is empty, oversized, malformed, or
/// uses an unsupported protocol version.
pub fn decode_request(payload: &[u8]) -> Result<Request, CodecError> {
    validate_payload(payload, MAX_REQUEST_BYTES)?;
    if payload.len() != 4 || payload[3] != 0 {
        return Err(CodecError::InvalidPayload);
    }
    validate_version(payload)?;
    match payload[2] {
        REQUEST_CAPABILITIES => Ok(Request::Capabilities),
        REQUEST_STATUS => Ok(Request::Status),
        _ => Err(CodecError::InvalidPayload),
    }
}

#[must_use]
pub fn encode_response(response: Response) -> Vec<u8> {
    let version = PROTOCOL_VERSION.to_be_bytes();
    let mut payload = vec![version[0], version[1], 0, 0];
    match response {
        Response::Capabilities(capabilities) => {
            payload[2] = RESPONSE_CAPABILITIES;
            payload.extend_from_slice(&capabilities.protocol_min.to_be_bytes());
            payload.extend_from_slice(&capabilities.protocol_max.to_be_bytes());
            payload.extend_from_slice(&capabilities.max_request_bytes.to_be_bytes());
            payload.extend_from_slice(&capabilities.max_response_bytes.to_be_bytes());
            payload.extend_from_slice(&capabilities.operations.to_be_bytes());
            payload.extend_from_slice(&support_bytes([
                capabilities.detector_analysis,
                capabilities.embedding_extraction,
                capabilities.user_session_enrollment,
                capabilities.encrypted_user_session_persistence,
                capabilities.explicit_local_verification,
                capabilities.profile_deletion,
                capabilities.pam_authentication,
                capabilities.pam_configuration,
                capabilities.authselect,
                capabilities.sddm_lock_screen,
                capabilities.sudo_polkit,
                capabilities.liveness,
                capabilities.security_tiers,
                capabilities.privileged_services,
            ]));
            payload.push(0);
            payload.push(0);
        }
        Response::Status(status) => {
            payload[2] = RESPONSE_STATUS;
            payload.push(2);
            payload.extend_from_slice(&support_bytes([
                status.detector_analysis,
                status.embedding_extraction,
                status.user_session_enrollment,
                status.encrypted_user_session_persistence,
                status.explicit_local_verification,
                status.profile_deletion,
                status.pam_authentication,
                status.pam_configuration,
                status.authselect,
                status.sddm_lock_screen,
                status.sudo_polkit,
                status.liveness,
                status.security_tiers,
                status.privileged_services,
            ]));
        }
        Response::Error(code) => {
            payload[2] = RESPONSE_ERROR;
            payload.push(match code {
                ErrorCode::InvalidRequest => 1,
                ErrorCode::UnsupportedVersion => 2,
                ErrorCode::UnsupportedOperation => 3,
            });
        }
    }
    payload
}

/// Decodes one bounded response payload.
///
/// # Errors
///
/// Returns [`CodecError`] when the payload is empty, oversized, malformed, or
/// uses an unsupported protocol version.
pub fn decode_response(payload: &[u8]) -> Result<Response, CodecError> {
    validate_payload(payload, MAX_RESPONSE_BYTES)?;
    if payload.len() < 5 || payload[3] != 0 {
        return Err(CodecError::InvalidPayload);
    }
    validate_version(payload)?;
    match payload[2] {
        RESPONSE_CAPABILITIES => decode_capabilities(payload),
        RESPONSE_STATUS => decode_status(payload),
        RESPONSE_ERROR if payload.len() == 5 => {
            let code = match payload[4] {
                1 => ErrorCode::InvalidRequest,
                2 => ErrorCode::UnsupportedVersion,
                3 => ErrorCode::UnsupportedOperation,
                _ => return Err(CodecError::InvalidPayload),
            };
            Ok(Response::Error(code))
        }
        _ => Err(CodecError::InvalidPayload),
    }
}

/// Reads one length-prefixed frame without allocating beyond `maximum`.
///
/// # Errors
///
/// Returns [`FrameError`] for I/O errors, zero-length frames, or declared
/// lengths greater than `maximum`.
pub fn read_frame<R: Read>(reader: &mut R, maximum: usize) -> Result<Vec<u8>, FrameError> {
    match read_frame_or_eof(reader, maximum)? {
        Some(payload) => Ok(payload),
        None => Err(FrameError::Io(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "bounded frame stream ended before a frame header",
        ))),
    }
}

/// Reads one length-prefixed frame and distinguishes clean stream EOF from a
/// truncated frame header.
///
/// # Errors
///
/// Returns [`FrameError`] for I/O errors, zero-length frames, truncated
/// headers, or declared lengths greater than `maximum`.
pub fn read_frame_or_eof<R: Read>(
    reader: &mut R,
    maximum: usize,
) -> Result<Option<Vec<u8>>, FrameError> {
    let mut header = [0_u8; 4];
    let mut offset = 0_usize;
    while offset < header.len() {
        match reader.read(&mut header[offset..]) {
            Ok(0) if offset == 0 => return Ok(None),
            Ok(0) => {
                return Err(FrameError::Io(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "bounded frame header is truncated",
                )));
            }
            Ok(bytes_read) => offset += bytes_read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(FrameError::Io(error)),
        }
    }

    let length = u32::from_be_bytes(header) as usize;
    if length == 0 {
        return Err(FrameError::EmptyFrame);
    }
    if length > maximum {
        return Err(FrameError::FrameTooLarge { length, maximum });
    }
    let mut payload = vec![0_u8; length];
    if let Err(error) = reader.read_exact(&mut payload) {
        payload.zeroize();
        return Err(FrameError::Io(error));
    }
    Ok(Some(payload))
}

/// Requires a bounded stream to contain exactly one frame.
///
/// # Errors
///
/// Returns [`FrameError::TrailingData`] when another byte is present.
pub fn require_eof<R: Read>(reader: &mut R) -> Result<(), FrameError> {
    let mut trailing = [0_u8; 1];
    loop {
        match reader.read(&mut trailing) {
            Ok(0) => return Ok(()),
            Ok(_) => return Err(FrameError::TrailingData),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(FrameError::Io(error)),
        }
    }
}

/// Writes one bounded length-prefixed frame.
///
/// # Errors
///
/// Returns [`FrameError`] for empty or oversized payloads and for I/O errors.
pub fn write_frame<W: Write>(
    writer: &mut W,
    payload: &[u8],
    maximum: usize,
) -> Result<(), FrameError> {
    if payload.is_empty() {
        return Err(FrameError::EmptyFrame);
    }
    if payload.len() > maximum {
        return Err(FrameError::FrameTooLarge {
            length: payload.len(),
            maximum,
        });
    }
    let length = u32::try_from(payload.len()).map_err(|_| FrameError::FrameTooLarge {
        length: payload.len(),
        maximum,
    })?;
    writer.write_all(&length.to_be_bytes())?;
    writer.write_all(payload)?;
    writer.flush()?;
    Ok(())
}

fn validate_payload(payload: &[u8], maximum: usize) -> Result<(), CodecError> {
    if payload.is_empty() {
        return Err(CodecError::EmptyFrame);
    }
    if payload.len() > maximum {
        return Err(CodecError::FrameTooLarge {
            length: payload.len(),
            maximum,
        });
    }
    Ok(())
}

fn validate_version(payload: &[u8]) -> Result<(), CodecError> {
    if payload.len() < 2 {
        return Err(CodecError::InvalidPayload);
    }
    let version = u16::from_be_bytes([payload[0], payload[1]]);
    if version != PROTOCOL_VERSION {
        return Err(CodecError::UnsupportedVersion(version));
    }
    Ok(())
}

const fn support_bytes(support: [OperationSupport; 14]) -> [u8; 14] {
    [
        support[0].wire_value(),
        support[1].wire_value(),
        support[2].wire_value(),
        support[3].wire_value(),
        support[4].wire_value(),
        support[5].wire_value(),
        support[6].wire_value(),
        support[7].wire_value(),
        support[8].wire_value(),
        support[9].wire_value(),
        support[10].wire_value(),
        support[11].wire_value(),
        support[12].wire_value(),
        support[13].wire_value(),
    ]
}

fn decode_support(payload: &[u8]) -> Result<[OperationSupport; 14], CodecError> {
    Ok([
        OperationSupport::from_wire(payload[0])?,
        OperationSupport::from_wire(payload[1])?,
        OperationSupport::from_wire(payload[2])?,
        OperationSupport::from_wire(payload[3])?,
        OperationSupport::from_wire(payload[4])?,
        OperationSupport::from_wire(payload[5])?,
        OperationSupport::from_wire(payload[6])?,
        OperationSupport::from_wire(payload[7])?,
        OperationSupport::from_wire(payload[8])?,
        OperationSupport::from_wire(payload[9])?,
        OperationSupport::from_wire(payload[10])?,
        OperationSupport::from_wire(payload[11])?,
        OperationSupport::from_wire(payload[12])?,
        OperationSupport::from_wire(payload[13])?,
    ])
}

fn decode_capabilities(payload: &[u8]) -> Result<Response, CodecError> {
    if payload.len() != 34 || payload[32] != 0 || payload[33] != 0 {
        return Err(CodecError::InvalidPayload);
    }
    let support = decode_support(&payload[18..32])?;
    Ok(Response::Capabilities(Capabilities {
        protocol_min: u16::from_be_bytes([payload[4], payload[5]]),
        protocol_max: u16::from_be_bytes([payload[6], payload[7]]),
        max_request_bytes: u32::from_be_bytes([payload[8], payload[9], payload[10], payload[11]]),
        max_response_bytes: u32::from_be_bytes([
            payload[12],
            payload[13],
            payload[14],
            payload[15],
        ]),
        operations: u16::from_be_bytes([payload[16], payload[17]]),
        detector_analysis: support[0],
        embedding_extraction: support[1],
        user_session_enrollment: support[2],
        encrypted_user_session_persistence: support[3],
        explicit_local_verification: support[4],
        profile_deletion: support[5],
        pam_authentication: support[6],
        pam_configuration: support[7],
        authselect: support[8],
        sddm_lock_screen: support[9],
        sudo_polkit: support[10],
        liveness: support[11],
        security_tiers: support[12],
        privileged_services: support[13],
        network_access: DisabledCapability::Disabled,
        runtime_model_downloads: DisabledCapability::Disabled,
    }))
}

fn decode_status(payload: &[u8]) -> Result<Response, CodecError> {
    if payload.len() != 19 || payload[4] != 2 {
        return Err(CodecError::InvalidPayload);
    }
    let support = decode_support(&payload[5..19])?;
    Ok(Response::Status(Status {
        state: EngineState::Ready,
        detector_analysis: support[0],
        embedding_extraction: support[1],
        user_session_enrollment: support[2],
        encrypted_user_session_persistence: support[3],
        explicit_local_verification: support[4],
        profile_deletion: support[5],
        pam_authentication: support[6],
        pam_configuration: support[7],
        authselect: support[8],
        sddm_lock_screen: support[9],
        sudo_polkit: support[10],
        liveness: support[11],
        security_tiers: support[12],
        privileged_services: support[13],
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn request_round_trips_are_closed() {
        for request in [Request::Capabilities, Request::Status] {
            assert_eq!(decode_request(&encode_request(request)).unwrap(), request);
        }
        assert!(matches!(
            decode_request(&[0, 2, 9, 0]),
            Err(CodecError::InvalidPayload)
        ));
    }

    #[test]
    fn responses_round_trip() {
        let responses = [
            Response::Capabilities(Capabilities::local_identity()),
            Response::Status(Status::local_identity()),
            Response::Error(ErrorCode::UnsupportedOperation),
        ];
        for response in responses {
            let encoded = encode_response(response);
            assert!(encoded.len() <= MAX_RESPONSE_BYTES);
            assert_eq!(decode_response(&encoded).unwrap(), response);
        }
    }

    #[test]
    fn oversized_frame_is_rejected_before_payload_read() {
        let header = u32::try_from(MAX_REQUEST_BYTES + 1).unwrap().to_be_bytes();
        let mut reader = Cursor::new(header);
        assert!(matches!(
            read_frame(&mut reader, MAX_REQUEST_BYTES),
            Err(FrameError::FrameTooLarge { .. })
        ));
        assert_eq!(reader.position(), 4);
    }

    #[test]
    fn fragmented_reads_and_writes_are_bounded() {
        let payload = encode_request(Request::Status);
        let mut framed = Vec::new();
        write_frame(&mut framed, &payload, MAX_REQUEST_BYTES).unwrap();
        let decoded = read_frame(&mut Cursor::new(framed), MAX_REQUEST_BYTES).unwrap();
        assert_eq!(decoded, payload);
    }

    #[test]
    fn zero_length_and_wrong_version_fail_closed() {
        assert!(matches!(
            read_frame(&mut Cursor::new([0, 0, 0, 0]), MAX_REQUEST_BYTES),
            Err(FrameError::EmptyFrame)
        ));
        assert!(matches!(
            decode_request(&[0, 3, REQUEST_STATUS, 0]),
            Err(CodecError::UnsupportedVersion(3))
        ));
    }

    #[test]
    fn clean_eof_and_partial_headers_are_distinct() {
        assert_eq!(
            read_frame_or_eof(&mut Cursor::new([]), MAX_REQUEST_BYTES).unwrap(),
            None
        );
        assert!(matches!(
            read_frame_or_eof(&mut Cursor::new([0, 0]), MAX_REQUEST_BYTES),
            Err(FrameError::Io(error)) if error.kind() == io::ErrorKind::UnexpectedEof
        ));
    }
}
