// SPDX-License-Identifier: GPL-3.0-or-later

//! Production `OpenCV` 4.13 `FaceDetectorYN` provider for the pinned `YuNet` model.

use std::fmt;
use std::path::Path;

use kfaceauth_vision_opencv_sys::{
    BridgeError, Detector, InferenceBackend, RawDetection, opencv_version,
};

use crate::model::{ManifestEntry, ModelError, VerifiedArtifact, load_and_verify_model_inventory};
use crate::{
    FaceObservation, FaceRectangle, ImageView, MAX_FACES, PixelFormat, ProcessingControl,
    VisionAnalysis, VisionError, VisionProvider, calculate_quality,
};

pub const YUNET_ARTIFACT_ID: &str = "yunet-2023mar";
pub const YUNET_MODEL_PATH: &str = "files/face_detection_yunet_2023mar.onnx";
pub const YUNET_MODEL_SIZE: u64 = 232_589;
pub const YUNET_MODEL_SHA256: &str =
    "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4";
pub const YUNET_SCORE_THRESHOLD: f32 = 0.9;
pub const YUNET_NMS_THRESHOLD: f32 = 0.3;
pub const YUNET_TOP_K: usize = 5_000;
pub const YUNET_MINIMUM_RUNTIME_DIMENSION: u32 = 64;

const INITIAL_WIDTH: u32 = 320;
const INITIAL_HEIGHT: u32 = 320;
pub const TRACKING_WIDTH: u32 = 320;
pub const TRACKING_HEIGHT: u32 = 320;
const SCORE_TOLERANCE: f32 = 1.0e-5;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum InferenceMode {
    Tracking,
    FullResolution,
}

pub struct YuNetProvider {
    detector: Detector,
}

impl YuNetProvider {
    /// Verifies the complete closed inventory, verifies the exact selected
    /// model artifact, and constructs the production `OpenCV` `YuNet` bridge.
    ///
    /// # Errors
    ///
    /// Returns a stable load error for any inventory, metadata, digest, or
    /// `OpenCV` initialization failure.
    pub fn from_model_root(root: &Path) -> Result<Self, ProviderLoadError> {
        let (manifest, artifacts) = load_and_verify_model_inventory(root)?;
        let entry = manifest
            .find(YUNET_ARTIFACT_ID)
            .ok_or(ProviderLoadError::UnexpectedModelMetadata)?;
        require_expected_metadata(entry)?;
        let artifact = artifacts
            .iter()
            .find(|a| a.entry().id == YUNET_ARTIFACT_ID)
            .ok_or(ProviderLoadError::UnexpectedModelMetadata)?;
        Self::from_verified_artifact(artifact)
    }

    /// Initializes a `YuNet` detector from an already verified artifact.
    ///
    /// # Errors
    ///
    /// Returns a stable load error if metadata is invalid or `OpenCV` runtime fails.
    pub fn from_verified_artifact(artifact: &VerifiedArtifact) -> Result<Self, ProviderLoadError> {
        require_expected_metadata(artifact.entry())?;
        let detector = Detector::new(
            artifact.bytes(),
            INITIAL_WIDTH,
            INITIAL_HEIGHT,
            YUNET_SCORE_THRESHOLD,
            YUNET_NMS_THRESHOLD,
            YUNET_TOP_K,
        )?;
        Ok(Self { detector })
    }

    #[must_use]
    pub fn runtime_version() -> String {
        opencv_version()
    }

    #[must_use]
    pub fn backend(&self) -> InferenceBackend {
        self.detector.backend()
    }

    #[must_use]
    pub fn uses_vulkan(&self) -> bool {
        self.backend() == InferenceBackend::Vulkan
    }

    pub(crate) fn detect_raw(
        &self,
        image: ImageView<'_>,
        control: ProcessingControl<'_>,
        mode: InferenceMode,
    ) -> Result<(RuntimeInput, SensitiveDetections, crate::QualityMetrics), VisionError> {
        control.check()?;
        let quality = calculate_quality(image, control)?;
        let bgr = convert_to_bgr(image, control)?;
        control.check()?;
        let stride = usize::try_from(bgr.width)
            .ok()
            .and_then(|width| width.checked_mul(3))
            .ok_or(VisionError::InvalidRuntimeOutput)?;
        let (inference_width, inference_height) = match mode {
            InferenceMode::Tracking => (TRACKING_WIDTH, TRACKING_HEIGHT),
            InferenceMode::FullResolution => (bgr.width, bgr.height),
        };
        let raw = self
            .detector
            .detect_at_resolution(
                &bgr.bytes.0,
                bgr.width,
                bgr.height,
                stride,
                (inference_width, inference_height),
                YUNET_TOP_K,
            )
            .map_err(map_bridge_error)?;
        let raw = SensitiveDetections(raw);
        control.check()?;
        validate_detections(&raw.0, image.width, image.height)?;
        control.check()?;
        Ok((bgr, raw, quality))
    }
}

impl VisionProvider for YuNetProvider {
    fn analyze(
        &self,
        image: ImageView<'_>,
        control: ProcessingControl<'_>,
    ) -> Result<VisionAnalysis, VisionError> {
        let (_bgr, raw, quality) = self.detect_raw(image, control, InferenceMode::Tracking)?;
        let faces = validate_detections(&raw.0, image.width, image.height)?;
        control.check()?;
        Ok(VisionAnalysis { faces, quality })
    }
}

#[derive(Debug)]
pub enum ProviderLoadError {
    Model(ModelError),
    UnexpectedModelMetadata,
    Runtime(BridgeError),
}

impl fmt::Display for ProviderLoadError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Model(error) => write!(formatter, "provider model load failed: {error}"),
            Self::UnexpectedModelMetadata => {
                formatter.write_str("YuNet manifest metadata does not match the reviewed model")
            }
            Self::Runtime(error) => {
                write!(formatter, "YuNet runtime initialization failed: {error}")
            }
        }
    }
}

impl std::error::Error for ProviderLoadError {}

impl From<ModelError> for ProviderLoadError {
    fn from(error: ModelError) -> Self {
        Self::Model(error)
    }
}

impl From<BridgeError> for ProviderLoadError {
    fn from(error: BridgeError) -> Self {
        Self::Runtime(error)
    }
}

fn require_expected_metadata(entry: &ManifestEntry) -> Result<(), ProviderLoadError> {
    if entry.id != YUNET_ARTIFACT_ID
        || entry.path != Path::new(YUNET_MODEL_PATH)
        || entry.size != YUNET_MODEL_SIZE
        || entry.sha256 != YUNET_MODEL_SHA256
        || entry.role != "detector"
        || entry.backend != "opencv-facedetectoryn"
        || entry.license != "MIT"
        || entry.provenance != "opencv-zoo-47534e27"
    {
        return Err(ProviderLoadError::UnexpectedModelMetadata);
    }
    Ok(())
}

pub(crate) struct SensitiveBytes(pub(crate) Vec<u8>);

impl Drop for SensitiveBytes {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}

pub(crate) struct RuntimeInput {
    pub(crate) bytes: SensitiveBytes,
    pub(crate) width: u32,
    pub(crate) height: u32,
}

pub(crate) struct SensitiveDetections(pub(crate) Vec<RawDetection>);

impl Drop for SensitiveDetections {
    fn drop(&mut self) {
        self.0.fill(RawDetection::default());
    }
}

fn convert_to_bgr(
    image: ImageView<'_>,
    control: ProcessingControl<'_>,
) -> Result<RuntimeInput, VisionError> {
    let width = usize::try_from(image.width).map_err(|_| VisionError::InvalidRuntimeOutput)?;
    let height = usize::try_from(image.height).map_err(|_| VisionError::InvalidRuntimeOutput)?;
    let runtime_width = usize::try_from(image.width.max(YUNET_MINIMUM_RUNTIME_DIMENSION))
        .map_err(|_| VisionError::InvalidRuntimeOutput)?;
    let runtime_height = usize::try_from(image.height.max(YUNET_MINIMUM_RUNTIME_DIMENSION))
        .map_err(|_| VisionError::InvalidRuntimeOutput)?;
    let source_stride =
        usize::try_from(image.stride).map_err(|_| VisionError::InvalidRuntimeOutput)?;
    let source_pixel_size = usize::try_from(image.format.bytes_per_pixel())
        .map_err(|_| VisionError::InvalidRuntimeOutput)?;
    let pixels = runtime_width
        .checked_mul(runtime_height)
        .ok_or(VisionError::InvalidRuntimeOutput)?;
    let output_size = pixels
        .checked_mul(3)
        .ok_or(VisionError::InvalidRuntimeOutput)?;
    let mut output = SensitiveBytes(vec![0; output_size]);

    for y in 0..height {
        control.check()?;
        let source_row = y
            .checked_mul(source_stride)
            .ok_or(VisionError::InvalidRuntimeOutput)?;
        let output_row = y
            .checked_mul(runtime_width)
            .and_then(|offset| offset.checked_mul(3))
            .ok_or(VisionError::InvalidRuntimeOutput)?;
        for x in 0..width {
            let source = source_row
                .checked_add(
                    x.checked_mul(source_pixel_size)
                        .ok_or(VisionError::InvalidRuntimeOutput)?,
                )
                .ok_or(VisionError::InvalidRuntimeOutput)?;
            let target = output_row
                .checked_add(x.checked_mul(3).ok_or(VisionError::InvalidRuntimeOutput)?)
                .ok_or(VisionError::InvalidRuntimeOutput)?;
            match image.format {
                PixelFormat::Rgb8 | PixelFormat::Rgba8 => {
                    output.0[target] = image.bytes[source + 2];
                    output.0[target + 1] = image.bytes[source + 1];
                    output.0[target + 2] = image.bytes[source];
                }
                PixelFormat::Gray8 => {
                    let value = image.bytes[source];
                    output.0[target..target + 3].fill(value);
                }
            }
        }
    }
    Ok(RuntimeInput {
        bytes: output,
        width: u32::try_from(runtime_width).map_err(|_| VisionError::InvalidRuntimeOutput)?,
        height: u32::try_from(runtime_height).map_err(|_| VisionError::InvalidRuntimeOutput)?,
    })
}

pub(crate) fn validate_detections(
    detections: &[RawDetection],
    image_width: u32,
    image_height: u32,
) -> Result<Vec<FaceObservation>, VisionError> {
    if detections.len() > YUNET_TOP_K {
        return Err(VisionError::InvalidRuntimeOutput);
    }
    let image_width_f32 =
        f32::from(u16::try_from(image_width).map_err(|_| VisionError::InvalidRuntimeOutput)?);
    let image_height_f32 =
        f32::from(u16::try_from(image_height).map_err(|_| VisionError::InvalidRuntimeOutput)?);
    let mut faces = Vec::with_capacity(detections.len().min(MAX_FACES));
    for detection in detections {
        if !detection.values.iter().all(|value| value.is_finite()) {
            return Err(VisionError::InvalidRuntimeOutput);
        }
        let x = detection.values[0];
        let y = detection.values[1];
        let width = detection.values[2];
        let height = detection.values[3];
        let right = x + width;
        let bottom = y + height;
        if x < 0.0
            || y < 0.0
            || width <= 0.0
            || height <= 0.0
            || !right.is_finite()
            || !bottom.is_finite()
            || right > image_width_f32
            || bottom > image_height_f32
        {
            return Err(VisionError::InvalidRuntimeOutput);
        }
        for landmark in detection.values[4..14].chunks_exact(2) {
            if landmark[0] < 0.0
                || landmark[0] >= image_width_f32
                || landmark[1] < 0.0
                || landmark[1] >= image_height_f32
            {
                return Err(VisionError::InvalidRuntimeOutput);
            }
        }
        let score = detection.values[14];
        if score + SCORE_TOLERANCE < YUNET_SCORE_THRESHOLD || score > 1.0 + SCORE_TOLERANCE {
            return Err(VisionError::InvalidRuntimeOutput);
        }

        let left = x.floor();
        let top = y.floor();
        let right = right.ceil();
        let bottom = bottom.ceil();
        let integer_width = right - left;
        let integer_height = bottom - top;
        if integer_width <= 0.0 || integer_height <= 0.0 {
            return Err(VisionError::InvalidRuntimeOutput);
        }
        if faces.len() < MAX_FACES {
            faces.push(FaceObservation {
                rectangle: FaceRectangle {
                    x: checked_u16(left)?,
                    y: checked_u16(top)?,
                    width: checked_u16(integer_width)?,
                    height: checked_u16(integer_height)?,
                },
            });
        }
    }
    Ok(faces)
}

#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
fn checked_u16(value: f32) -> Result<u16, VisionError> {
    if !value.is_finite() || value < 0.0 || value > f32::from(u16::MAX) {
        return Err(VisionError::InvalidRuntimeOutput);
    }
    Ok(value as u16)
}

fn map_bridge_error(error: BridgeError) -> VisionError {
    match error {
        BridgeError::MalformedOutput
        | BridgeError::OutputTooLarge
        | BridgeError::InvalidArgument => VisionError::InvalidRuntimeOutput,
        BridgeError::RuntimeFailure
        | BridgeError::HardeningFailure
        | BridgeError::UnknownStatus => VisionError::RuntimeFailure,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::CancellationToken;
    use std::time::Duration;

    fn control(token: &CancellationToken) -> ProcessingControl<'_> {
        ProcessingControl::with_timeout(token, Duration::from_secs(10)).unwrap()
    }

    fn valid_detection() -> RawDetection {
        RawDetection {
            values: [
                10.25, 12.5, 20.0, 30.0, 15.0, 18.0, 24.0, 18.0, 20.0, 24.0, 16.0, 34.0, 24.0,
                34.0, 0.95,
            ],
        }
    }

    #[test]
    fn converts_rgb_rgba_gray_and_padded_stride_to_tight_bgr() {
        let token = CancellationToken::default();
        let rgb = ImageView::new(PixelFormat::Rgb8, 1, 1, 3, &[1, 2, 3]).unwrap();
        let rgb = convert_to_bgr(rgb, control(&token)).unwrap();
        assert_eq!(&rgb.bytes.0[..3], [3, 2, 1]);
        assert!(rgb.bytes.0[3..].iter().all(|byte| *byte == 0));

        let rgba = ImageView::new(PixelFormat::Rgba8, 1, 1, 4, &[4, 5, 6, 99]).unwrap();
        let rgba = convert_to_bgr(rgba, control(&token)).unwrap();
        assert_eq!(&rgba.bytes.0[..3], [6, 5, 4]);
        assert!(rgba.bytes.0[3..].iter().all(|byte| *byte == 0));

        let gray = ImageView::new(PixelFormat::Gray8, 2, 1, 4, &[7, 8, 99, 99]).unwrap();
        let gray = convert_to_bgr(gray, control(&token)).unwrap();
        assert_eq!(&gray.bytes.0[..6], [7, 7, 7, 8, 8, 8]);
        assert!(gray.bytes.0[6..].iter().all(|byte| *byte == 0));
    }

    #[test]
    fn accepts_smallest_and_largest_conversion_geometry() {
        let token = CancellationToken::default();
        let smallest = ImageView::new(PixelFormat::Gray8, 1, 1, 1, &[0]).unwrap();
        let smallest = convert_to_bgr(smallest, control(&token)).unwrap();
        assert_eq!(smallest.width, YUNET_MINIMUM_RUNTIME_DIMENSION);
        assert_eq!(smallest.height, YUNET_MINIMUM_RUNTIME_DIMENSION);
        assert_eq!(smallest.bytes.0.len(), 64 * 64 * 3);

        let largest_bytes = vec![0; 1920 * 1080 * 4];
        let largest =
            ImageView::new(PixelFormat::Rgba8, 1920, 1080, 1920 * 4, &largest_bytes).unwrap();
        let largest = convert_to_bgr(largest, control(&token)).unwrap();
        assert_eq!(largest.width, 1920);
        assert_eq!(largest.height, 1080);
        assert_eq!(largest.bytes.0.len(), 1920 * 1080 * 3);
    }

    #[test]
    fn validates_every_runtime_value_and_bounds() {
        assert_eq!(
            validate_detections(&[valid_detection()], 64, 64)
                .unwrap()
                .len(),
            1
        );

        let mut nan = valid_detection();
        nan.values[14] = f32::NAN;
        assert_eq!(
            validate_detections(&[nan], 64, 64),
            Err(VisionError::InvalidRuntimeOutput)
        );
        let mut infinite = valid_detection();
        infinite.values[2] = f32::INFINITY;
        assert_eq!(
            validate_detections(&[infinite], 64, 64),
            Err(VisionError::InvalidRuntimeOutput)
        );
        let mut negative = valid_detection();
        negative.values[0] = -0.1;
        assert_eq!(
            validate_detections(&[negative], 64, 64),
            Err(VisionError::InvalidRuntimeOutput)
        );
        let mut rectangle_outside = valid_detection();
        rectangle_outside.values[2] = 60.0;
        assert_eq!(
            validate_detections(&[rectangle_outside], 64, 64),
            Err(VisionError::InvalidRuntimeOutput)
        );
        let mut landmark_outside = valid_detection();
        landmark_outside.values[4] = 64.0;
        assert_eq!(
            validate_detections(&[landmark_outside], 64, 64),
            Err(VisionError::InvalidRuntimeOutput)
        );
    }

    #[test]
    fn enforces_score_threshold_and_eight_face_limit() {
        let mut below_threshold = valid_detection();
        below_threshold.values[14] = YUNET_SCORE_THRESHOLD - 0.01;
        assert_eq!(
            validate_detections(&[below_threshold], 64, 64),
            Err(VisionError::InvalidRuntimeOutput)
        );
        assert_eq!(
            validate_detections(&[valid_detection(); MAX_FACES + 1], 64, 64)
                .unwrap()
                .len(),
            MAX_FACES
        );
        assert!((YUNET_SCORE_THRESHOLD - 0.9).abs() < f32::EPSILON);
        assert!((YUNET_NMS_THRESHOLD - 0.3).abs() < f32::EPSILON);
        assert_eq!(YUNET_TOP_K, 5_000);
    }

    #[test]
    fn initializes_verified_model_and_runs_smallest_and_largest_real_zero_face_inference() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../models");
        let provider = YuNetProvider::from_model_root(&root).unwrap();
        let token = CancellationToken::default();
        let smallest_bytes = [0; 3];
        let smallest = ImageView::new(PixelFormat::Rgb8, 1, 1, 3, &smallest_bytes).unwrap();
        assert!(
            provider
                .analyze(smallest, control(&token))
                .unwrap()
                .faces
                .is_empty()
        );

        let largest_bytes = vec![0; 1920 * 1080 * 3];
        let largest =
            ImageView::new(PixelFormat::Rgb8, 1920, 1080, 1920 * 3, &largest_bytes).unwrap();
        assert!(
            provider
                .analyze(largest, control(&token))
                .unwrap()
                .faces
                .is_empty()
        );
    }
}
