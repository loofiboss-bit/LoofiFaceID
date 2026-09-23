// SPDX-License-Identifier: GPL-3.0-or-later

//! Bounded YuNet-to-SFace identity feature extraction.
//!
//! This module produces sensitive normalized embeddings for the private
//! identity worker boundary. It does not persist profiles, choose an
//! authorization result, or expose features to presentation code.

use std::fmt;
use std::path::Path;

pub use kfaceauth_identity_types::{
    DETECTOR_MODEL_ID, EMBEDDING_DIMENSION, EMBEDDING_FORMAT_ID, EMBEDDING_MODEL_ID,
    NORMALIZATION_VERSION, NormalizedEmbedding, SFACE_MODEL_SHA256, cosine_similarity,
};
use kfaceauth_vision_opencv_sys::{
    BridgeError, Recognizer, SFACE_ALIGNED_HEIGHT, SFACE_ALIGNED_WIDTH, SFACE_EMBEDDING_DIMENSION,
};

use crate::model::{ManifestEntry, ModelError, load_and_verify_model_inventory};
use crate::yunet::{InferenceMode, ProviderLoadError as DetectorLoadError, YuNetProvider};
use crate::{ImageView, ProcessingControl, VisionError};

pub const SFACE_ARTIFACT_ID: &str = "sface-2021dec";
pub const SFACE_MODEL_PATH: &str = "files/face_recognition_sface_2021dec.onnx";
pub const SFACE_MODEL_SIZE: u64 = 38_696_353;
pub const ALIGNED_WIDTH: u32 = SFACE_ALIGNED_WIDTH;
pub const ALIGNED_HEIGHT: u32 = SFACE_ALIGNED_HEIGHT;

const MINIMUM_FACE_EDGE: f32 = 80.0;
const EDGE_MARGIN: f32 = 4.0;

const _: [(); SFACE_EMBEDDING_DIMENSION] = [(); EMBEDDING_DIMENSION];

pub struct IdentityProvider {
    detector: YuNetProvider,
    recognizer: Recognizer,
}

impl IdentityProvider {
    /// Verifies the complete inventory and exact detector/embedding identities
    /// before initializing either `OpenCV` object from verified bytes.
    ///
    /// # Errors
    ///
    /// Returns a fail-closed load error for inventory, metadata, or runtime
    /// failures.
    pub fn from_model_root(root: &Path) -> Result<Self, IdentityLoadError> {
        let (manifest, artifacts) = load_and_verify_model_inventory(root)?;
        let entry = manifest
            .find(SFACE_ARTIFACT_ID)
            .ok_or(IdentityLoadError::UnexpectedModelMetadata)?;
        require_expected_metadata(entry)?;
        let yunet_artifact = artifacts
            .iter()
            .find(|a| a.entry().id == crate::yunet::YUNET_ARTIFACT_ID)
            .ok_or(IdentityLoadError::UnexpectedModelMetadata)?;
        let detector = YuNetProvider::from_verified_artifact(yunet_artifact)
            .map_err(|_| IdentityLoadError::UnexpectedModelMetadata)?;
        let sface_artifact = artifacts
            .iter()
            .find(|a| a.entry().id == SFACE_ARTIFACT_ID)
            .ok_or(IdentityLoadError::UnexpectedModelMetadata)?;
        require_expected_metadata(sface_artifact.entry())?;
        let recognizer = Recognizer::new(sface_artifact.bytes())?;
        Ok(Self {
            detector,
            recognizer,
        })
    }

    /// Extracts one normalized embedding from exactly one acceptable face.
    ///
    /// The returned value is biometric material and must remain inside the
    /// private identity/vault boundary.
    ///
    /// # Errors
    ///
    /// Rejects zero/multiple faces, poor quality, invalid face geometry,
    /// cancellation, timeout, and malformed native output.
    pub fn extract(
        &self,
        image: ImageView<'_>,
        control: ProcessingControl<'_>,
    ) -> Result<NormalizedEmbedding, IdentityError> {
        let (bgr, mut detections, quality) =
            self.detector
                .detect_raw(image, control, InferenceMode::FullResolution)?;
        if detections.0.is_empty() {
            let (_, tracking_detections, _) =
                self.detector
                    .detect_raw(image, control, InferenceMode::Tracking)?;
            detections = tracking_detections;
        }
        if detections.0.is_empty() {
            return Err(IdentityError::NoFace);
        }
        if detections.0.len() != 1 {
            return Err(IdentityError::MultipleFaces);
        }
        if quality.flags != 0 {
            return Err(IdentityError::PoorQuality);
        }
        let detection = &detections.0[0];
        let [x, y, width, height, ..] = detection.values;
        let image_width =
            f32::from(u16::try_from(image.width).map_err(|_| IdentityError::InvalidEmbedding)?);
        let image_height =
            f32::from(u16::try_from(image.height).map_err(|_| IdentityError::InvalidEmbedding)?);
        if width < MINIMUM_FACE_EDGE
            || height < MINIMUM_FACE_EDGE
            || x < EDGE_MARGIN
            || y < EDGE_MARGIN
            || x + width > image_width - EDGE_MARGIN
            || y + height > image_height - EDGE_MARGIN
        {
            return Err(IdentityError::FaceGeometry);
        }
        control.check()?;
        let stride = usize::try_from(bgr.width)
            .ok()
            .and_then(|width| width.checked_mul(3))
            .ok_or(IdentityError::InvalidEmbedding)?;

        let pad_decision = crate::liveness::PresentationAttackDetector::evaluate_single_frame(
            &bgr.bytes.0,
            bgr.width,
            bgr.height,
            stride,
            detection,
            None,
        );
        match pad_decision {
            crate::liveness::LivenessDecision::SpoofDetected(spoof) => {
                return Err(IdentityError::SpoofDetected(spoof));
            }
            crate::liveness::LivenessDecision::AnalysisFailed => {
                return Err(IdentityError::LivenessUnavailable);
            }
            crate::liveness::LivenessDecision::BonaFide
            | crate::liveness::LivenessDecision::AwaitingChallenge(_) => {}
        }

        let raw = self
            .recognizer
            .extract(&bgr.bytes.0, bgr.width, bgr.height, stride, detection)
            .map_err(IdentityError::Runtime)?;
        control.check()?;
        let embedding =
            NormalizedEmbedding::from_raw(raw).map_err(|_| IdentityError::InvalidEmbedding)?;
        control.check()?;
        Ok(embedding)
    }

    /// Checks the safe-Rust cosine implementation against `OpenCV` for tests and
    /// developer qualification. Policy decisions use [`cosine_similarity`].
    ///
    /// # Errors
    ///
    /// Returns a stable bridge error for native failure.
    pub fn native_cosine(
        &self,
        left: &NormalizedEmbedding,
        right: &NormalizedEmbedding,
    ) -> Result<f64, BridgeError> {
        self.recognizer
            .cosine(left.sensitive_values(), right.sensitive_values())
    }
}

#[derive(Debug)]
pub enum IdentityLoadError {
    Model(ModelError),
    Detector(DetectorLoadError),
    UnexpectedModelMetadata,
    Runtime(BridgeError),
}

impl fmt::Display for IdentityLoadError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Model(error) => write!(formatter, "identity model load failed: {error}"),
            Self::Detector(error) => write!(formatter, "identity detector load failed: {error}"),
            Self::UnexpectedModelMetadata => {
                formatter.write_str("SFace manifest metadata does not match the reviewed model")
            }
            Self::Runtime(error) => {
                write!(formatter, "SFace runtime initialization failed: {error}")
            }
        }
    }
}

impl std::error::Error for IdentityLoadError {}

impl From<ModelError> for IdentityLoadError {
    fn from(error: ModelError) -> Self {
        Self::Model(error)
    }
}

impl From<DetectorLoadError> for IdentityLoadError {
    fn from(error: DetectorLoadError) -> Self {
        Self::Detector(error)
    }
}

impl From<BridgeError> for IdentityLoadError {
    fn from(error: BridgeError) -> Self {
        Self::Runtime(error)
    }
}

#[derive(Debug)]
pub enum IdentityError {
    Cancelled,
    DeadlineExceeded,
    NoFace,
    MultipleFaces,
    PoorQuality,
    FaceGeometry,
    LivenessUnavailable,
    InvalidEmbedding,
    SpoofDetected(crate::liveness::SpoofKind),
    Runtime(BridgeError),
}

impl fmt::Display for IdentityError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Cancelled => formatter.write_str("identity extraction was cancelled"),
            Self::DeadlineExceeded => {
                formatter.write_str("identity extraction deadline was exceeded")
            }
            Self::NoFace => formatter.write_str("exactly one face is required"),
            Self::MultipleFaces => formatter.write_str("multiple faces are not accepted"),
            Self::PoorQuality => {
                formatter.write_str("image quality is outside the accepted bounds")
            }
            Self::FaceGeometry => {
                formatter.write_str("face size or edge position is outside the accepted bounds")
            }
            Self::LivenessUnavailable => {
                formatter.write_str("presentation attack analysis could not be completed")
            }
            Self::InvalidEmbedding => {
                formatter.write_str("embedding output violated the identity contract")
            }
            Self::SpoofDetected(spoof) => write!(
                formatter,
                "presentation attack detected: {}",
                match spoof {
                    crate::liveness::SpoofKind::PrintAttack => "printed photo attack",
                    crate::liveness::SpoofKind::ScreenReplay => "screen replay attack",
                    crate::liveness::SpoofKind::UnnaturalBlink => "unnatural blink",
                    crate::liveness::SpoofKind::PoseChallengeFailed => "pose challenge failed",
                    crate::liveness::SpoofKind::LowNirReflectance => "low NIR reflectance",
                }
            ),
            Self::Runtime(_) => formatter.write_str("identity runtime failed"),
        }
    }
}

impl std::error::Error for IdentityError {}

impl From<VisionError> for IdentityError {
    fn from(error: VisionError) -> Self {
        match error {
            VisionError::Cancelled => Self::Cancelled,
            VisionError::DeadlineExceeded => Self::DeadlineExceeded,
            VisionError::FaceAtEdge => Self::FaceGeometry,
            VisionError::RuntimeFailure => Self::Runtime(BridgeError::RuntimeFailure),
            VisionError::InvalidRuntimeOutput => Self::InvalidEmbedding,
        }
    }
}

fn require_expected_metadata(entry: &ManifestEntry) -> Result<(), IdentityLoadError> {
    if entry.id != SFACE_ARTIFACT_ID
        || entry.path != Path::new(SFACE_MODEL_PATH)
        || entry.size != SFACE_MODEL_SIZE
        || entry.sha256 != SFACE_MODEL_SHA256
        || entry.role != "embedding"
        || entry.backend != "opencv-facerecognizersf"
        || entry.license != "Apache-2.0"
        || entry.provenance != "opencv-zoo-47534e27"
    {
        return Err(IdentityLoadError::UnexpectedModelMetadata);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{IdentityError, VisionError};

    #[test]
    fn clipped_detector_geometry_is_rejected_as_face_geometry() {
        assert!(matches!(
            IdentityError::from(VisionError::FaceAtEdge),
            IdentityError::FaceGeometry
        ));
    }
}
