// SPDX-License-Identifier: GPL-3.0-or-later

use std::fmt;
use std::io::{self, Read, Write};
use std::time::Duration;

use crate::{
    CancellationToken, ImageError, ImageView, MAX_FACES, MAX_FRAME_BYTES, PixelFormat,
    ProcessingControl, VisionAnalysis, VisionError, VisionProvider,
};

pub const WORKER_PROTOCOL_VERSION: u16 = 1;
pub const OP_ANALYZE: u8 = 1;
pub const RESPONSE_ANALYSIS: u8 = 0x81;
pub const RESPONSE_ERROR: u8 = 0xff;
pub const REQUEST_HEADER_BYTES: usize = 24;
pub const MAX_REQUEST_PAYLOAD: usize = MAX_FRAME_BYTES + REQUEST_HEADER_BYTES;
pub const MAX_RESPONSE_PAYLOAD: usize = 16 + MAX_FACES * 8;
pub const MIN_TIMEOUT_MS: u32 = 1;
pub const MAX_TIMEOUT_MS: u32 = 5_000;

/// Stable worker error-code map. Numeric values are part of protocol version 1.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum WorkerErrorCode {
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    UnsupportedOperation = 3,
    UnsupportedPixelFormat = 4,
    InvalidDimensions = 5,
    InvalidStride = 6,
    InvalidFrameSize = 7,
    InvalidTimeout = 8,
    Cancelled = 9,
    DeadlineExceeded = 10,
    ModelUnavailable = 11,
    InternalError = 12,
}

#[derive(Debug)]
pub enum WorkerIoError {
    Io(io::Error),
    EmptyFrame,
    FrameTooLarge,
    TrailingData,
}

impl fmt::Display for WorkerIoError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(formatter, "vision worker I/O failed: {error}"),
            Self::EmptyFrame => formatter.write_str("vision worker frame is empty"),
            Self::FrameTooLarge => formatter.write_str("vision worker frame is oversized"),
            Self::TrailingData => formatter.write_str("vision worker received trailing data"),
        }
    }
}

impl std::error::Error for WorkerIoError {}

impl From<io::Error> for WorkerIoError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

#[derive(Clone, Copy, Debug)]
struct AnalyzeRequest<'a> {
    generation: u64,
    timeout_ms: u32,
    image: ImageView<'a>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct RequestError {
    code: WorkerErrorCode,
    generation: u64,
}

/// Reads one request, writes one response, and returns. The caller must exit
/// the worker process after this function, regardless of success or failure.
///
/// # Errors
///
/// Returns [`WorkerIoError`] for invalid framing or stream I/O. Decodable
/// payload errors are returned on the wire using a stable error code.
pub fn serve_once_with_provider<R: Read, W: Write, P: VisionProvider>(
    reader: &mut R,
    writer: &mut W,
    provider: &P,
) -> Result<(), WorkerIoError> {
    let mut payload = read_frame(reader)?;
    if let Err(error) = require_eof(reader) {
        payload.fill(0);
        return Err(error);
    }
    let response = match parse_request(&payload) {
        Ok(request) => process_request(provider, request),
        Err(error) => encode_error(error.code, error.generation),
    };
    payload.fill(0);
    write_frame(writer, &response)
}

/// Reads one request and initializes the production provider only after the
/// request has passed framing and protocol validation.
///
/// This permits model/runtime initialization failures to be returned as a
/// stable framed error instead of looking like an unexplained worker crash.
///
/// # Errors
///
/// Returns [`WorkerIoError`] for invalid framing or stream I/O.
pub fn serve_once_with_provider_factory<R, W, P, F>(
    reader: &mut R,
    writer: &mut W,
    factory: F,
) -> Result<(), WorkerIoError>
where
    R: Read,
    W: Write,
    P: VisionProvider,
    F: FnOnce() -> Result<P, WorkerErrorCode>,
{
    let mut payload = read_frame(reader)?;
    if let Err(error) = require_eof(reader) {
        payload.fill(0);
        return Err(error);
    }
    let response = match parse_request(&payload) {
        Ok(request) => match factory() {
            Ok(provider) => process_request(&provider, request),
            Err(code) => encode_error(code, request.generation),
        },
        Err(error) => encode_error(error.code, error.generation),
    };
    payload.fill(0);
    write_frame(writer, &response)
}

/// Serves a continuous stream of vision analysis requests over a persistent session
/// until the input stream reaches EOF.
///
/// # Errors
///
/// Returns [`WorkerIoError`] for I/O errors or malformed frame framing.
pub fn serve_session_with_provider<R: Read, W: Write, P: VisionProvider>(
    reader: &mut R,
    writer: &mut W,
    provider: &P,
) -> Result<(), WorkerIoError> {
    loop {
        let mut header = [0_u8; 4];
        match reader.read_exact(&mut header) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(error) => return Err(WorkerIoError::Io(error)),
        }
        let length = u32::from_be_bytes(header) as usize;
        if length == 0 {
            return Err(WorkerIoError::EmptyFrame);
        }
        if length > MAX_REQUEST_PAYLOAD {
            return Err(WorkerIoError::FrameTooLarge);
        }
        let mut payload = vec![0_u8; length];
        if let Err(error) = reader.read_exact(&mut payload) {
            payload.fill(0);
            return Err(WorkerIoError::Io(error));
        }
        let response = match parse_request(&payload) {
            Ok(request) => process_request(provider, request),
            Err(error) => encode_error(error.code, error.generation),
        };
        payload.fill(0);
        write_frame(writer, &response)?;
    }
}

/// Serves a persistent session with lazy initialization of the provider on the first request.
///
/// # Errors
///
/// Returns [`WorkerIoError`] for framing or stream I/O failures.
pub fn serve_session_with_provider_factory<R, W, P, F>(
    reader: &mut R,
    writer: &mut W,
    factory: F,
) -> Result<(), WorkerIoError>
where
    R: Read,
    W: Write,
    P: VisionProvider,
    F: FnOnce() -> Result<P, WorkerErrorCode>,
{
    let mut header = [0_u8; 4];
    match reader.read_exact(&mut header) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
        Err(error) => return Err(WorkerIoError::Io(error)),
    }
    let length = u32::from_be_bytes(header) as usize;
    if length == 0 {
        return Err(WorkerIoError::EmptyFrame);
    }
    if length > MAX_REQUEST_PAYLOAD {
        return Err(WorkerIoError::FrameTooLarge);
    }
    let mut payload = vec![0_u8; length];
    if let Err(error) = reader.read_exact(&mut payload) {
        payload.fill(0);
        return Err(WorkerIoError::Io(error));
    }
    let (provider, response) = match parse_request(&payload) {
        Ok(request) => match factory() {
            Ok(p) => {
                let resp = process_request(&p, request);
                (Some(p), resp)
            }
            Err(code) => (None, encode_error(code, request.generation)),
        },
        Err(error) => (None, encode_error(error.code, error.generation)),
    };
    payload.fill(0);
    write_frame(writer, &response)?;

    if let Some(p) = provider {
        serve_session_with_provider(reader, writer, &p)
    } else {
        Ok(())
    }
}

fn process_request(provider: &dyn VisionProvider, request: AnalyzeRequest<'_>) -> Vec<u8> {
    let cancellation = CancellationToken::default();
    let control = match ProcessingControl::with_timeout(
        &cancellation,
        Duration::from_millis(u64::from(request.timeout_ms)),
    ) {
        Ok(control) => control,
        Err(error) => {
            return encode_error(map_vision_error(error), request.generation);
        }
    };
    match provider.analyze(request.image, control) {
        Ok(analysis) if valid_analysis(&analysis, request.image) => {
            encode_analysis(request.generation, &analysis)
        }
        Ok(_) => encode_error(WorkerErrorCode::InternalError, request.generation),
        Err(error) => encode_error(map_vision_error(error), request.generation),
    }
}

fn valid_analysis(analysis: &VisionAnalysis, image: ImageView<'_>) -> bool {
    if analysis.faces.len() > MAX_FACES
        || analysis.quality.flags
            & !(crate::QUALITY_TOO_DARK
                | crate::QUALITY_TOO_BRIGHT
                | crate::QUALITY_LOW_CONTRAST
                | crate::QUALITY_LOW_SHARPNESS)
            != 0
        || analysis.quality.flags & crate::QUALITY_TOO_DARK != 0
            && analysis.quality.flags & crate::QUALITY_TOO_BRIGHT != 0
    {
        return false;
    }
    analysis.faces.iter().all(|face| {
        let rectangle = face.rectangle;
        rectangle.width > 0
            && rectangle.height > 0
            && u32::from(rectangle.x)
                .checked_add(u32::from(rectangle.width))
                .is_some_and(|right| right <= image.width)
            && u32::from(rectangle.y)
                .checked_add(u32::from(rectangle.height))
                .is_some_and(|bottom| bottom <= image.height)
    })
}

fn parse_request(payload: &[u8]) -> Result<AnalyzeRequest<'_>, RequestError> {
    let generation = payload
        .get(4..12)
        .and_then(|bytes| bytes.try_into().ok())
        .map_or(0, u64::from_be_bytes);
    if payload.len() < REQUEST_HEADER_BYTES {
        return Err(RequestError {
            code: WorkerErrorCode::InvalidRequest,
            generation,
        });
    }

    let version = u16::from_be_bytes(payload[0..2].try_into().expect("checked header length"));
    if version != WORKER_PROTOCOL_VERSION {
        return Err(RequestError {
            code: WorkerErrorCode::UnsupportedVersion,
            generation,
        });
    }
    if generation == 0 {
        return Err(RequestError {
            code: WorkerErrorCode::InvalidRequest,
            generation,
        });
    }
    if payload[2] != OP_ANALYZE {
        return Err(RequestError {
            code: WorkerErrorCode::UnsupportedOperation,
            generation,
        });
    }
    let format = PixelFormat::try_from(payload[3]).map_err(|_| RequestError {
        code: WorkerErrorCode::UnsupportedPixelFormat,
        generation,
    })?;
    let timeout_ms = u32::from_be_bytes(payload[12..16].try_into().expect("checked header length"));
    if !(MIN_TIMEOUT_MS..=MAX_TIMEOUT_MS).contains(&timeout_ms) {
        return Err(RequestError {
            code: WorkerErrorCode::InvalidTimeout,
            generation,
        });
    }
    let width = u32::from(u16::from_be_bytes(
        payload[16..18].try_into().expect("checked header length"),
    ));
    let height = u32::from(u16::from_be_bytes(
        payload[18..20].try_into().expect("checked header length"),
    ));
    let stride = u32::from_be_bytes(payload[20..24].try_into().expect("checked header length"));
    let image = ImageView::new(
        format,
        width,
        height,
        stride,
        &payload[REQUEST_HEADER_BYTES..],
    )
    .map_err(|error| RequestError {
        code: map_image_error(error),
        generation,
    })?;
    Ok(AnalyzeRequest {
        generation,
        timeout_ms,
        image,
    })
}

fn map_image_error(error: ImageError) -> WorkerErrorCode {
    match error {
        ImageError::UnsupportedPixelFormat => WorkerErrorCode::UnsupportedPixelFormat,
        ImageError::InvalidDimensions => WorkerErrorCode::InvalidDimensions,
        ImageError::InvalidStride => WorkerErrorCode::InvalidStride,
        ImageError::ArithmeticOverflow | ImageError::FrameTooLarge | ImageError::LengthMismatch => {
            WorkerErrorCode::InvalidFrameSize
        }
    }
}

const fn map_vision_error(error: VisionError) -> WorkerErrorCode {
    match error {
        VisionError::Cancelled => WorkerErrorCode::Cancelled,
        VisionError::DeadlineExceeded => WorkerErrorCode::DeadlineExceeded,
        VisionError::RuntimeFailure | VisionError::InvalidRuntimeOutput => {
            WorkerErrorCode::InternalError
        }
    }
}

fn encode_analysis(generation: u64, analysis: &VisionAnalysis) -> Vec<u8> {
    let face_count = u8::try_from(analysis.faces.len()).unwrap_or(u8::MAX);
    if usize::from(face_count) > MAX_FACES {
        return encode_error(WorkerErrorCode::InternalError, generation);
    }
    let mut payload = Vec::with_capacity(16 + analysis.faces.len() * 8);
    payload.extend_from_slice(&WORKER_PROTOCOL_VERSION.to_be_bytes());
    payload.push(RESPONSE_ANALYSIS);
    payload.push(face_count);
    payload.extend_from_slice(&generation.to_be_bytes());
    payload.push(analysis.quality.brightness);
    payload.push(analysis.quality.contrast);
    payload.push(analysis.quality.sharpness);
    payload.push(analysis.quality.flags);
    for face in &analysis.faces {
        payload.extend_from_slice(&face.rectangle.x.to_be_bytes());
        payload.extend_from_slice(&face.rectangle.y.to_be_bytes());
        payload.extend_from_slice(&face.rectangle.width.to_be_bytes());
        payload.extend_from_slice(&face.rectangle.height.to_be_bytes());
    }
    payload
}

#[must_use]
pub fn encode_error(code: WorkerErrorCode, generation: u64) -> Vec<u8> {
    let mut payload = Vec::with_capacity(12);
    payload.extend_from_slice(&WORKER_PROTOCOL_VERSION.to_be_bytes());
    payload.push(RESPONSE_ERROR);
    payload.push(code as u8);
    payload.extend_from_slice(&generation.to_be_bytes());
    payload
}

fn read_frame<R: Read>(reader: &mut R) -> Result<Vec<u8>, WorkerIoError> {
    let mut header = [0_u8; 4];
    reader.read_exact(&mut header)?;
    let length = u32::from_be_bytes(header) as usize;
    if length == 0 {
        return Err(WorkerIoError::EmptyFrame);
    }
    if length > MAX_REQUEST_PAYLOAD {
        return Err(WorkerIoError::FrameTooLarge);
    }
    let mut payload = vec![0_u8; length];
    if let Err(error) = reader.read_exact(&mut payload) {
        payload.fill(0);
        return Err(WorkerIoError::Io(error));
    }
    Ok(payload)
}

fn require_eof<R: Read>(reader: &mut R) -> Result<(), WorkerIoError> {
    let mut trailing = [0_u8; 1];
    loop {
        match reader.read(&mut trailing) {
            Ok(0) => return Ok(()),
            Ok(_) => {
                trailing.fill(0);
                return Err(WorkerIoError::TrailingData);
            }
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(WorkerIoError::Io(error)),
        }
    }
}

fn write_frame<W: Write>(writer: &mut W, payload: &[u8]) -> Result<(), WorkerIoError> {
    if payload.is_empty() || payload.len() > MAX_RESPONSE_PAYLOAD {
        return Err(WorkerIoError::FrameTooLarge);
    }
    let length = u32::try_from(payload.len()).map_err(|_| WorkerIoError::FrameTooLarge)?;
    writer.write_all(&length.to_be_bytes())?;
    writer.write_all(payload)?;
    writer.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{FakeDeterministicProvider, VisionAnalysis};
    use std::io::Cursor;

    fn provider() -> FakeDeterministicProvider {
        FakeDeterministicProvider::new()
    }

    fn framed(payload: &[u8]) -> Vec<u8> {
        let mut result = Vec::new();
        result.extend_from_slice(
            &u32::try_from(payload.len())
                .expect("test payload length fits u32")
                .to_be_bytes(),
        );
        result.extend_from_slice(payload);
        result
    }

    fn analyze_request(format: PixelFormat, pixels: &[u8]) -> Vec<u8> {
        let mut payload = Vec::new();
        payload.extend_from_slice(&WORKER_PROTOCOL_VERSION.to_be_bytes());
        payload.push(OP_ANALYZE);
        payload.push(format as u8);
        payload.extend_from_slice(&42_u64.to_be_bytes());
        payload.extend_from_slice(&1_000_u32.to_be_bytes());
        payload.extend_from_slice(&2_u16.to_be_bytes());
        payload.extend_from_slice(&1_u16.to_be_bytes());
        payload.extend_from_slice(&(2 * format.bytes_per_pixel()).to_be_bytes());
        payload.extend_from_slice(pixels);
        payload
    }

    #[test]
    fn exact_success_layout_echoes_generation() {
        let request = analyze_request(PixelFormat::Gray8, &[1, 33]);
        let mut output = Vec::new();
        serve_once_with_provider(&mut Cursor::new(framed(&request)), &mut output, &provider())
            .unwrap();
        let declared = u32::from_be_bytes(output[0..4].try_into().unwrap()) as usize;
        assert_eq!(declared, output.len() - 4);
        assert_eq!(&output[4..6], &WORKER_PROTOCOL_VERSION.to_be_bytes());
        assert_eq!(output[6], RESPONSE_ANALYSIS);
        assert_eq!(output[7], 1);
        assert_eq!(u64::from_be_bytes(output[8..16].try_into().unwrap()), 42);
        assert_eq!(declared, 24);
    }

    #[test]
    fn malformed_length_is_a_stable_error_response() {
        let mut request = analyze_request(PixelFormat::Gray8, &[1]);
        request[4..12].copy_from_slice(&99_u64.to_be_bytes());
        let mut output = Vec::new();
        serve_once_with_provider(&mut Cursor::new(framed(&request)), &mut output, &provider())
            .unwrap();
        assert_eq!(output[6], RESPONSE_ERROR);
        assert_eq!(output[7], WorkerErrorCode::InvalidFrameSize as u8);
        assert_eq!(u64::from_be_bytes(output[8..16].try_into().unwrap()), 99);
    }

    #[test]
    fn rejects_zero_generation_and_trailing_requests() {
        let mut request = analyze_request(PixelFormat::Gray8, &[1, 33]);
        request[4..12].fill(0);
        let error = parse_request(&request).unwrap_err();
        assert_eq!(error.code, WorkerErrorCode::InvalidRequest);
        assert_eq!(error.generation, 0);

        let request = analyze_request(PixelFormat::Gray8, &[1, 33]);
        let mut with_trailing_request = framed(&request);
        with_trailing_request.extend_from_slice(&framed(&request));
        let mut output = Vec::new();
        assert!(matches!(
            serve_once_with_provider(
                &mut Cursor::new(with_trailing_request),
                &mut output,
                &provider()
            ),
            Err(WorkerIoError::TrailingData)
        ));
        assert!(output.is_empty());
    }

    #[test]
    fn rejects_unsupported_version_operation_format_and_timeout() {
        let base = analyze_request(PixelFormat::Gray8, &[1, 33]);
        for (offset, value, code) in [
            (1, 2, WorkerErrorCode::UnsupportedVersion),
            (2, 2, WorkerErrorCode::UnsupportedOperation),
            (3, 9, WorkerErrorCode::UnsupportedPixelFormat),
        ] {
            let mut request = base.clone();
            request[offset] = value;
            let error = parse_request(&request).unwrap_err();
            assert_eq!(error.code, code);
            assert_eq!(error.generation, 42);
        }
        let mut invalid_timeout = base;
        invalid_timeout[12..16].fill(0);
        let error = parse_request(&invalid_timeout).unwrap_err();
        assert_eq!(error.code, WorkerErrorCode::InvalidTimeout);
        assert_eq!(error.generation, 42);
    }

    #[test]
    fn rejects_oversized_frame_before_allocation() {
        let mut input = Cursor::new(
            u32::try_from(MAX_REQUEST_PAYLOAD + 1)
                .unwrap()
                .to_be_bytes(),
        );
        let error = serve_once_with_provider(&mut input, &mut Vec::new(), &provider()).unwrap_err();
        assert!(matches!(error, WorkerIoError::FrameTooLarge));
    }

    #[test]
    fn provider_initialization_failure_is_a_framed_model_error() {
        let request = analyze_request(PixelFormat::Gray8, &[1, 33]);
        let mut output = Vec::new();
        serve_once_with_provider_factory::<_, _, FakeDeterministicProvider, _>(
            &mut Cursor::new(framed(&request)),
            &mut output,
            || Err(WorkerErrorCode::ModelUnavailable),
        )
        .unwrap();
        assert_eq!(output[6], RESPONSE_ERROR);
        assert_eq!(output[7], WorkerErrorCode::ModelUnavailable as u8);
        assert_eq!(u64::from_be_bytes(output[8..16].try_into().unwrap()), 42);
    }

    struct InvalidOutputProvider;

    impl VisionProvider for InvalidOutputProvider {
        fn analyze(
            &self,
            _image: ImageView<'_>,
            _control: ProcessingControl<'_>,
        ) -> Result<VisionAnalysis, VisionError> {
            Err(VisionError::InvalidRuntimeOutput)
        }
    }

    #[test]
    fn invalid_runtime_output_maps_to_internal_error() {
        let request = analyze_request(PixelFormat::Gray8, &[1, 33]);
        let mut output = Vec::new();
        serve_once_with_provider(
            &mut Cursor::new(framed(&request)),
            &mut output,
            &InvalidOutputProvider,
        )
        .unwrap();
        assert_eq!(output[6], RESPONSE_ERROR);
        assert_eq!(output[7], WorkerErrorCode::InternalError as u8);
    }
}
