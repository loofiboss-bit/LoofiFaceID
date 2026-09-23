// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

pub mod identity;
pub mod liveness;
pub mod model;
mod sha256;
pub mod worker;
pub mod yunet;

use std::fmt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

pub const MAX_WIDTH: u32 = 1920;
pub const MAX_HEIGHT: u32 = 1080;
pub const MAX_FRAME_BYTES: usize = 1920 * 1080 * 4;
pub const MAX_FACES: usize = 8;

pub const QUALITY_TOO_DARK: u8 = 1 << 0;
pub const QUALITY_TOO_BRIGHT: u8 = 1 << 1;
pub const QUALITY_LOW_CONTRAST: u8 = 1 << 2;
pub const QUALITY_LOW_SHARPNESS: u8 = 1 << 3;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum PixelFormat {
    Rgb8 = 1,
    Rgba8 = 2,
    Gray8 = 3,
}

impl PixelFormat {
    #[must_use]
    pub const fn bytes_per_pixel(self) -> u32 {
        match self {
            Self::Rgb8 => 3,
            Self::Rgba8 => 4,
            Self::Gray8 => 1,
        }
    }
}

impl TryFrom<u8> for PixelFormat {
    type Error = ImageError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::Rgb8),
            2 => Ok(Self::Rgba8),
            3 => Ok(Self::Gray8),
            _ => Err(ImageError::UnsupportedPixelFormat),
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ImageView<'a> {
    pub format: PixelFormat,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub bytes: &'a [u8],
}

impl<'a> ImageView<'a> {
    /// Constructs a bounded image view with checked geometry and exact length.
    ///
    /// # Errors
    ///
    /// Returns [`ImageError`] for zero or oversized dimensions, a short
    /// stride, arithmetic overflow, an oversized frame, or a length mismatch.
    pub fn new(
        format: PixelFormat,
        width: u32,
        height: u32,
        stride: u32,
        bytes: &'a [u8],
    ) -> Result<Self, ImageError> {
        if width == 0 || height == 0 || width > MAX_WIDTH || height > MAX_HEIGHT {
            return Err(ImageError::InvalidDimensions);
        }
        let minimum_stride = width
            .checked_mul(format.bytes_per_pixel())
            .ok_or(ImageError::ArithmeticOverflow)?;
        if stride < minimum_stride {
            return Err(ImageError::InvalidStride);
        }
        let expected_u32 = stride
            .checked_mul(height)
            .ok_or(ImageError::ArithmeticOverflow)?;
        let expected = usize::try_from(expected_u32).map_err(|_| ImageError::ArithmeticOverflow)?;
        if expected > MAX_FRAME_BYTES {
            return Err(ImageError::FrameTooLarge);
        }
        if bytes.len() != expected {
            return Err(ImageError::LengthMismatch);
        }
        Ok(Self {
            format,
            width,
            height,
            stride,
            bytes,
        })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImageError {
    UnsupportedPixelFormat,
    InvalidDimensions,
    InvalidStride,
    ArithmeticOverflow,
    FrameTooLarge,
    LengthMismatch,
}

impl fmt::Display for ImageError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::UnsupportedPixelFormat => "pixel format is unsupported",
            Self::InvalidDimensions => "image dimensions are invalid",
            Self::InvalidStride => "image stride is invalid",
            Self::ArithmeticOverflow => "image geometry overflows",
            Self::FrameTooLarge => "image frame is too large",
            Self::LengthMismatch => "image frame length does not match its geometry",
        })
    }
}

impl std::error::Error for ImageError {}

#[derive(Debug, Default)]
pub struct CancellationToken {
    cancelled: AtomicBool,
}

impl CancellationToken {
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    #[must_use]
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ProcessingControl<'a> {
    cancellation: &'a CancellationToken,
    deadline: Instant,
}

impl<'a> ProcessingControl<'a> {
    #[must_use]
    pub const fn new(cancellation: &'a CancellationToken, deadline: Instant) -> Self {
        Self {
            cancellation,
            deadline,
        }
    }

    /// Creates a control with a deadline relative to the current instant.
    ///
    /// # Errors
    ///
    /// Returns [`VisionError::DeadlineExceeded`] if the duration cannot be
    /// represented by [`Instant`].
    pub fn with_timeout(
        cancellation: &'a CancellationToken,
        timeout: Duration,
    ) -> Result<Self, VisionError> {
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or(VisionError::DeadlineExceeded)?;
        Ok(Self::new(cancellation, deadline))
    }

    /// Checks cancellation before the deadline.
    ///
    /// # Errors
    ///
    /// Returns a typed cancellation or deadline error.
    pub fn check(self) -> Result<(), VisionError> {
        if self.cancellation.is_cancelled() {
            return Err(VisionError::Cancelled);
        }
        if Instant::now() >= self.deadline {
            return Err(VisionError::DeadlineExceeded);
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FaceRectangle {
    pub x: u16,
    pub y: u16,
    pub width: u16,
    pub height: u16,
}

/// Five `YuNet` landmarks in source-image coordinates: right eye, left eye,
/// nose, right mouth corner, and left mouth corner. They stay inside the
/// private vision boundary and are never serialized into QML.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FaceLandmarks {
    pub points: [u16; 10],
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FaceObservation {
    pub rectangle: FaceRectangle,
    pub landmarks: FaceLandmarks,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct QualityMetrics {
    pub brightness: u8,
    pub contrast: u8,
    pub sharpness: u8,
    pub flags: u8,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VisionAnalysis {
    pub faces: Vec<FaceObservation>,
    pub quality: QualityMetrics,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum VisionError {
    Cancelled,
    DeadlineExceeded,
    FaceAtEdge,
    RuntimeFailure,
    InvalidRuntimeOutput,
}

impl fmt::Display for VisionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Cancelled => "vision processing was cancelled",
            Self::DeadlineExceeded => "vision processing deadline was exceeded",
            Self::FaceAtEdge => "face detection intersects the image boundary",
            Self::RuntimeFailure => "vision runtime failed",
            Self::InvalidRuntimeOutput => "vision runtime returned invalid output",
        })
    }
}

impl std::error::Error for VisionError {}

pub trait VisionProvider {
    /// Produces observations and quality only. This interface has no identity,
    /// matching, liveness, enrollment, or authentication result.
    ///
    /// # Errors
    ///
    /// Returns a typed cancellation or deadline error.
    fn analyze(
        &self,
        image: ImageView<'_>,
        control: ProcessingControl<'_>,
    ) -> Result<VisionAnalysis, VisionError>;
}

#[cfg(test)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FakeDeterministicProvider {
    max_faces: u8,
}

#[cfg(test)]
impl FakeDeterministicProvider {
    fn new() -> Self {
        Self { max_faces: 2 }
    }
}

#[cfg(test)]
impl VisionProvider for FakeDeterministicProvider {
    fn analyze(
        &self,
        image: ImageView<'_>,
        control: ProcessingControl<'_>,
    ) -> Result<VisionAnalysis, VisionError> {
        control.check()?;
        let stride = usize::try_from(image.stride).expect("validated stride fits usize");
        let width = usize::try_from(image.width).expect("bounded width fits usize");
        let height = usize::try_from(image.height).expect("bounded height fits usize");
        let bytes_per_pixel =
            usize::try_from(image.format.bytes_per_pixel()).expect("pixel size fits usize");

        let mut first_luma = None;
        for y in 0..height {
            control.check()?;
            let row = y * stride;
            for x in 0..width {
                let offset = row + x * bytes_per_pixel;
                let luma = pixel_luma(image.format, &image.bytes[offset..]);
                first_luma.get_or_insert(luma);
            }
        }
        control.check()?;

        let face_count = first_luma.unwrap_or(0) % (self.max_faces + 1);
        let faces = fake_rectangles(face_count, image.width, image.height);
        Ok(VisionAnalysis {
            faces,
            quality: calculate_quality(image, control)?,
        })
    }
}

pub(crate) fn pixel_luma(format: PixelFormat, bytes: &[u8]) -> u8 {
    match format {
        PixelFormat::Rgb8 | PixelFormat::Rgba8 => {
            let weighted =
                u32::from(bytes[0]) * 77 + u32::from(bytes[1]) * 150 + u32::from(bytes[2]) * 29;
            u8::try_from(weighted >> 8).expect("weighted RGB luma fits u8")
        }
        PixelFormat::Gray8 => bytes[0],
    }
}

pub(crate) fn calculate_quality(
    image: ImageView<'_>,
    control: ProcessingControl<'_>,
) -> Result<QualityMetrics, VisionError> {
    let stride = usize::try_from(image.stride).expect("validated stride fits usize");
    let width = usize::try_from(image.width).expect("bounded width fits usize");
    let height = usize::try_from(image.height).expect("bounded height fits usize");
    let bytes_per_pixel =
        usize::try_from(image.format.bytes_per_pixel()).expect("pixel size fits usize");
    let mut sum = 0_u64;
    let mut minimum = u8::MAX;
    let mut maximum = u8::MIN;
    let mut difference_sum = 0_u64;
    let mut difference_count = 0_u64;
    for y in 0..height {
        control.check()?;
        let row = y * stride;
        let mut previous = None;
        for x in 0..width {
            let offset = row + x * bytes_per_pixel;
            let luma = pixel_luma(image.format, &image.bytes[offset..]);
            sum += u64::from(luma);
            minimum = minimum.min(luma);
            maximum = maximum.max(luma);
            if let Some(previous) = previous {
                difference_sum += u64::from(luma.abs_diff(previous));
                difference_count += 1;
            }
            previous = Some(luma);
        }
    }
    control.check()?;

    let pixels = u64::try_from(width * height).expect("bounded pixel count fits u64");
    let brightness = u8::try_from(sum / pixels).expect("average luma is at most 255");
    let contrast = maximum - minimum;
    let sharpness = difference_sum
        .checked_div(difference_count)
        .map_or(0, |average| {
            u8::try_from(average).expect("average difference is at most 255")
        });
    let mut flags = 0;
    if brightness < 40 {
        flags |= QUALITY_TOO_DARK;
    }
    if brightness > 215 {
        flags |= QUALITY_TOO_BRIGHT;
    }
    if contrast < 16 {
        flags |= QUALITY_LOW_CONTRAST;
    }
    if sharpness < 4 {
        flags |= QUALITY_LOW_SHARPNESS;
    }
    Ok(QualityMetrics {
        brightness,
        contrast,
        sharpness,
        flags,
    })
}

#[cfg(test)]
fn fake_rectangles(count: u8, width: u32, height: u32) -> Vec<FaceObservation> {
    let rectangle_width = (width / 5).max(1);
    let rectangle_height = (height / 3).max(1);
    (0..count)
        .map(|index| {
            let column = u32::from(index % 4);
            let row = u32::from(index / 4);
            let x = ((column * width) / 4).min(width - 1);
            let y = ((row * height) / 2).min(height - 1);
            FaceObservation {
                rectangle: FaceRectangle {
                    x: u16::try_from(x).expect("bounded x fits u16"),
                    y: u16::try_from(y).expect("bounded y fits u16"),
                    width: u16::try_from(rectangle_width.min(width - x))
                        .expect("bounded width fits u16"),
                    height: u16::try_from(rectangle_height.min(height - y))
                        .expect("bounded height fits u16"),
                },
                landmarks: FaceLandmarks {
                    points: [
                        u16::try_from(x + rectangle_width / 3).expect("landmark fits u16"),
                        u16::try_from(y + rectangle_height / 3).expect("landmark fits u16"),
                        u16::try_from(x + (rectangle_width * 2) / 3).expect("landmark fits u16"),
                        u16::try_from(y + rectangle_height / 3).expect("landmark fits u16"),
                        u16::try_from(x + rectangle_width / 2).expect("landmark fits u16"),
                        u16::try_from(y + rectangle_height / 2).expect("landmark fits u16"),
                        u16::try_from(x + rectangle_width / 3).expect("landmark fits u16"),
                        u16::try_from(y + (rectangle_height * 3) / 4).expect("landmark fits u16"),
                        u16::try_from(x + (rectangle_width * 2) / 3).expect("landmark fits u16"),
                        u16::try_from(y + (rectangle_height * 3) / 4).expect("landmark fits u16"),
                    ],
                },
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_all_pixel_formats_and_exact_lengths() {
        assert!(ImageView::new(PixelFormat::Rgb8, 2, 2, 6, &[0; 12]).is_ok());
        assert!(ImageView::new(PixelFormat::Rgba8, 2, 2, 8, &[0; 16]).is_ok());
        assert!(ImageView::new(PixelFormat::Gray8, 2, 2, 2, &[0; 4]).is_ok());
        assert_eq!(
            ImageView::new(PixelFormat::Rgb8, 2, 2, 5, &[0; 10]).unwrap_err(),
            ImageError::InvalidStride
        );
        assert_eq!(
            ImageView::new(PixelFormat::Rgb8, 2, 2, 6, &[0; 11]).unwrap_err(),
            ImageError::LengthMismatch
        );
    }

    #[test]
    fn rejects_dimensions_and_oversized_padding() {
        assert_eq!(
            ImageView::new(PixelFormat::Gray8, 1921, 1, 1921, &[0; 1921]).unwrap_err(),
            ImageError::InvalidDimensions
        );
        assert_eq!(
            ImageView::new(PixelFormat::Gray8, 640, 480, u32::MAX, &[]).unwrap_err(),
            ImageError::ArithmeticOverflow
        );
    }

    #[test]
    fn cancellation_and_expired_deadline_are_typed() {
        let token = CancellationToken::default();
        token.cancel();
        let control = ProcessingControl::new(&token, Instant::now() + Duration::from_secs(1));
        assert_eq!(control.check(), Err(VisionError::Cancelled));

        let active = CancellationToken::default();
        let expired = ProcessingControl::new(&active, Instant::now());
        assert_eq!(expired.check(), Err(VisionError::DeadlineExceeded));
    }

    #[test]
    fn fake_provider_returns_zero_one_and_multiple_without_identity() {
        let provider = FakeDeterministicProvider::new();
        let cancellation = CancellationToken::default();
        for (marker, expected) in [(0_u8, 0_usize), (1, 1), (2, 2)] {
            let bytes = [marker, marker.wrapping_add(32)];
            let image = ImageView::new(PixelFormat::Gray8, 2, 1, 2, &bytes).unwrap();
            let control =
                ProcessingControl::with_timeout(&cancellation, Duration::from_secs(1)).unwrap();
            let result = provider.analyze(image, control).unwrap();
            assert_eq!(result.faces.len(), expected);
        }
    }
}
