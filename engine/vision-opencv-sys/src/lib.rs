// SPDX-License-Identifier: GPL-3.0-or-later

//! Narrow safe wrapper around the reviewed KFaceAuth/OpenCV C ABI.
//!
//! This crate is the only Rust package permitted to contain `unsafe` code.
//! C++ objects and OpenCV types never cross the boundary.

#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::fmt;
use std::marker::PhantomData;
use std::path::Path;
use std::ptr::{NonNull, null};
use std::rc::Rc;

use zeroize::Zeroize;

const STATUS_OK: c_int = 0;
const STATUS_INVALID_ARGUMENT: c_int = 1;
const STATUS_RUNTIME_FAILURE: c_int = 2;
const STATUS_OUTPUT_TOO_LARGE: c_int = 3;
const STATUS_MALFORMED_OUTPUT: c_int = 4;
const STATUS_HARDENING_FAILURE: c_int = 5;
const SANDBOX_UNAVAILABLE: c_int = 0;
const SANDBOX_APPLIED: c_int = 1;
const SANDBOX_ALREADY_APPLIED: c_int = 2;
const SANDBOX_FAILURE: c_int = 3;

const BACKEND_CPU: c_int = 0;
const BACKEND_OPENVINO: c_int = 1;
const BACKEND_VULKAN: c_int = 2;

pub const SFACE_EMBEDDING_DIMENSION: usize = 128;
pub const SFACE_ALIGNED_WIDTH: u32 = 112;
pub const SFACE_ALIGNED_HEIGHT: u32 = 112;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RawDetection {
    pub values: [f32; 15],
}

unsafe extern "C" {
    fn kfaceauth_yunet_disable_core_dumps() -> c_int;
    fn kfaceauth_yunet_opencv_version() -> *const c_char;
    fn kfaceauth_yunet_set_thread_count(thread_count: i32) -> c_int;
    fn kfaceauth_yunet_thread_count() -> i32;
    fn kfaceauth_yunet_configure_worker_sandbox(
        model_root: *const c_char,
        writable_root: *const c_char,
    ) -> c_int;
    fn kfaceauth_yunet_install_seccomp(allow_drm_ioctl: c_int) -> c_int;
    fn kfaceauth_yunet_backend(engine: *mut c_void) -> c_int;
    fn kfaceauth_yunet_create(
        model_bytes: *const u8,
        model_size: usize,
        width: i32,
        height: i32,
        score_threshold: f32,
        nms_threshold: f32,
        top_k: i32,
        detector_out: *mut *mut c_void,
    ) -> c_int;
    fn kfaceauth_yunet_detect_scaled(
        detector: *mut c_void,
        bgr_bytes: *const u8,
        bgr_size: usize,
        width: i32,
        height: i32,
        stride: usize,
        inference_width: i32,
        inference_height: i32,
        detections: *mut RawDetection,
        detection_capacity: usize,
        detection_count: *mut usize,
    ) -> c_int;
    fn kfaceauth_yunet_destroy(detector: *mut c_void);
    fn kfaceauth_sface_create(
        model_bytes: *const u8,
        model_size: usize,
        recognizer_out: *mut *mut c_void,
    ) -> c_int;
    fn kfaceauth_sface_extract(
        recognizer: *mut c_void,
        bgr_bytes: *const u8,
        bgr_size: usize,
        width: i32,
        height: i32,
        stride: usize,
        detection: *const RawDetection,
        embedding: *mut f32,
        embedding_capacity: usize,
        embedding_count: *mut usize,
    ) -> c_int;
    fn kfaceauth_sface_cosine(
        recognizer: *mut c_void,
        left: *const f32,
        left_count: usize,
        right: *const f32,
        right_count: usize,
        similarity: *mut f64,
    ) -> c_int;
    fn kfaceauth_sface_backend(recognizer: *mut c_void) -> c_int;
    fn kfaceauth_sface_destroy(recognizer: *mut c_void);
    fn kfaceauth_estimate_head_pose(
        detection: *const RawDetection,
        image_width: i32,
        image_height: i32,
        pose_out: *mut HeadPose,
    ) -> c_int;
    fn kfaceauth_analyze_texture(
        bgr_bytes: *const u8,
        bgr_size: usize,
        width: i32,
        height: i32,
        stride: usize,
        detection: *const RawDetection,
        metrics_out: *mut TextureMetrics,
    ) -> c_int;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BridgeError {
    InvalidArgument,
    RuntimeFailure,
    OutputTooLarge,
    MalformedOutput,
    HardeningFailure,
    UnknownStatus,
}

impl fmt::Display for BridgeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::InvalidArgument => "OpenCV bridge rejected an invalid argument",
            Self::RuntimeFailure => "OpenCV YuNet runtime failed",
            Self::OutputTooLarge => "OpenCV YuNet returned too many detections",
            Self::MalformedOutput => "OpenCV YuNet returned malformed output",
            Self::HardeningFailure => "vision worker core-dump hardening failed",
            Self::UnknownStatus => "OpenCV bridge returned an unknown status",
        })
    }
}

impl std::error::Error for BridgeError {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum InferenceBackend {
    Cpu = BACKEND_CPU,
    OpenVino = BACKEND_OPENVINO,
    Vulkan = BACKEND_VULKAN,
}

impl InferenceBackend {
    fn from_raw(value: c_int) -> Self {
        match value {
            BACKEND_OPENVINO => Self::OpenVino,
            BACKEND_VULKAN => Self::Vulkan,
            BACKEND_CPU => Self::Cpu,
            _ => Self::Cpu,
        }
    }

    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Cpu => "cpu",
            Self::OpenVino => "openvino",
            Self::Vulkan => "vulkan",
        }
    }

    #[must_use]
    pub const fn allows_drm_ioctl(self) -> bool {
        matches!(self, Self::Vulkan)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SandboxStatus {
    Unavailable,
    Applied,
    AlreadyApplied,
}

fn status_result(status: c_int) -> Result<(), BridgeError> {
    match status {
        STATUS_OK => Ok(()),
        STATUS_INVALID_ARGUMENT => Err(BridgeError::InvalidArgument),
        STATUS_RUNTIME_FAILURE => Err(BridgeError::RuntimeFailure),
        STATUS_OUTPUT_TOO_LARGE => Err(BridgeError::OutputTooLarge),
        STATUS_MALFORMED_OUTPUT => Err(BridgeError::MalformedOutput),
        STATUS_HARDENING_FAILURE => Err(BridgeError::HardeningFailure),
        _ => Err(BridgeError::UnknownStatus),
    }
}

/// Disables core dumps for the current short-lived worker process.
///
/// # Errors
///
/// Returns a stable bridge error if the operating system refuses hardening.
pub fn disable_core_dumps() -> Result<(), BridgeError> {
    // SAFETY: this function takes no pointers and has no caller-owned state.
    status_result(unsafe { kfaceauth_yunet_disable_core_dumps() })
}

/// Caps OpenCV's global worker pool to a bounded value for inference.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidArgument`] outside the reviewed range 1..=16.
pub fn set_thread_count(thread_count: u32) -> Result<(), BridgeError> {
    let thread_count = i32::try_from(thread_count).map_err(|_| BridgeError::InvalidArgument)?;
    status_result(unsafe { kfaceauth_yunet_set_thread_count(thread_count) })
}

#[must_use]
pub fn thread_count() -> u32 {
    let count = unsafe { kfaceauth_yunet_thread_count() };
    u32::try_from(count).unwrap_or(0)
}

/// Applies the optional Landlock worker policy before OpenCV initializes an
/// accelerator. `writable_root` is an explicit application-data subtree for
/// workers that must commit local state. Unsupported kernels are reported as
/// [`SandboxStatus::Unavailable`] so the caller can retain the verified CPU
/// path.
pub fn configure_worker_sandbox(
    model_root: &Path,
    writable_root: Option<&Path>,
) -> Result<SandboxStatus, BridgeError> {
    let root = model_root.to_str().ok_or(BridgeError::InvalidArgument)?;
    let root = CString::new(root).map_err(|_| BridgeError::InvalidArgument)?;
    let writable = writable_root
        .map(|path| path.to_str().ok_or(BridgeError::InvalidArgument))
        .transpose()?
        .map(CString::new)
        .transpose()
        .map_err(|_| BridgeError::InvalidArgument)?;
    let writable_ptr = writable.as_ref().map_or(null(), |value| value.as_ptr());
    let status = unsafe { kfaceauth_yunet_configure_worker_sandbox(root.as_ptr(), writable_ptr) };
    match status {
        SANDBOX_UNAVAILABLE => Ok(SandboxStatus::Unavailable),
        SANDBOX_APPLIED => Ok(SandboxStatus::Applied),
        SANDBOX_ALREADY_APPLIED => Ok(SandboxStatus::AlreadyApplied),
        SANDBOX_FAILURE => Err(BridgeError::HardeningFailure),
        _ => Err(BridgeError::UnknownStatus),
    }
}

/// Installs the opt-in syscall policy after model/provider initialization.
///
/// # Errors
///
/// Returns [`BridgeError::HardeningFailure`] if the kernel rejects the policy.
pub fn install_seccomp(allow_drm_ioctl: bool) -> Result<(), BridgeError> {
    status_result(unsafe { kfaceauth_yunet_install_seccomp(if allow_drm_ioctl { 1 } else { 0 }) })
}

#[must_use]
pub fn opencv_version() -> String {
    // SAFETY: OpenCV returns a process-lifetime static NUL-terminated string.
    let pointer = unsafe { kfaceauth_yunet_opencv_version() };
    if pointer.is_null() {
        return "unknown".to_owned();
    }
    // SAFETY: the bridge contract guarantees a valid static C string.
    unsafe { CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

pub struct Detector {
    handle: NonNull<c_void>,
    _not_send_or_sync: PhantomData<Rc<()>>,
}

impl Detector {
    /// Constructs a YuNet detector from already verified bytes.
    ///
    /// # Errors
    ///
    /// Returns a stable bridge error for rejected parameters or runtime load
    /// failures.
    pub fn new(
        model_bytes: &[u8],
        width: u32,
        height: u32,
        score_threshold: f32,
        nms_threshold: f32,
        top_k: usize,
    ) -> Result<Self, BridgeError> {
        let width = i32::try_from(width).map_err(|_| BridgeError::InvalidArgument)?;
        let height = i32::try_from(height).map_err(|_| BridgeError::InvalidArgument)?;
        let top_k = i32::try_from(top_k).map_err(|_| BridgeError::InvalidArgument)?;
        let mut handle = std::ptr::null_mut();
        // SAFETY: slices provide valid pointers and lengths for the duration
        // of the call; the output pointer refers to local initialized storage.
        status_result(unsafe {
            kfaceauth_yunet_create(
                model_bytes.as_ptr(),
                model_bytes.len(),
                width,
                height,
                score_threshold,
                nms_threshold,
                top_k,
                &mut handle,
            )
        })?;
        let handle = NonNull::new(handle).ok_or(BridgeError::RuntimeFailure)?;
        Ok(Self {
            handle,
            _not_send_or_sync: PhantomData,
        })
    }

    #[must_use]
    pub fn backend(&self) -> InferenceBackend {
        InferenceBackend::from_raw(unsafe { kfaceauth_yunet_backend(self.handle.as_ptr()) })
    }

    /// Runs one bounded BGR frame and returns raw 15-float YuNet rows.
    ///
    /// # Errors
    ///
    /// Returns a stable bridge error for invalid geometry, runtime failure, or
    /// malformed output.
    pub fn detect(
        &self,
        bgr_bytes: &[u8],
        width: u32,
        height: u32,
        stride: usize,
        maximum_detections: usize,
    ) -> Result<Vec<RawDetection>, BridgeError> {
        self.detect_at_resolution(
            bgr_bytes,
            width,
            height,
            stride,
            (width, height),
            maximum_detections,
        )
    }

    /// Runs one BGR frame at a bounded inference resolution and maps output
    /// coordinates back to the source frame. Passing the source dimensions
    /// selects full-resolution inference.
    pub fn detect_at_resolution(
        &self,
        bgr_bytes: &[u8],
        width: u32,
        height: u32,
        stride: usize,
        inference_size: (u32, u32),
        maximum_detections: usize,
    ) -> Result<Vec<RawDetection>, BridgeError> {
        let width = i32::try_from(width).map_err(|_| BridgeError::InvalidArgument)?;
        let height = i32::try_from(height).map_err(|_| BridgeError::InvalidArgument)?;
        let inference_width =
            i32::try_from(inference_size.0).map_err(|_| BridgeError::InvalidArgument)?;
        let inference_height =
            i32::try_from(inference_size.1).map_err(|_| BridgeError::InvalidArgument)?;
        if maximum_detections == 0 || maximum_detections > 5_000 {
            return Err(BridgeError::InvalidArgument);
        }
        let mut detections = vec![RawDetection::default(); maximum_detections];
        let mut count = 0_usize;
        // SAFETY: all buffers remain alive and uniquely writable as required
        // for the call; the opaque handle originated from the same bridge.
        status_result(unsafe {
            kfaceauth_yunet_detect_scaled(
                self.handle.as_ptr(),
                bgr_bytes.as_ptr(),
                bgr_bytes.len(),
                width,
                height,
                stride,
                if inference_width == width && inference_height == height {
                    0
                } else {
                    inference_width
                },
                if inference_width == width && inference_height == height {
                    0
                } else {
                    inference_height
                },
                detections.as_mut_ptr(),
                detections.len(),
                &mut count,
            )
        })?;
        if count > detections.len() {
            detections.fill(RawDetection::default());
            return Err(BridgeError::MalformedOutput);
        }
        detections.truncate(count);
        Ok(detections)
    }
}

impl Drop for Detector {
    fn drop(&mut self) {
        // SAFETY: the handle is uniquely owned and destroyed exactly once.
        unsafe { kfaceauth_yunet_destroy(self.handle.as_ptr()) };
    }
}

pub struct Recognizer {
    handle: NonNull<c_void>,
    _not_send_or_sync: PhantomData<Rc<()>>,
}

impl Recognizer {
    /// Constructs an SFace recognizer from already verified bytes.
    ///
    /// # Errors
    ///
    /// Returns a stable bridge error for rejected model bytes or runtime load
    /// failures.
    pub fn new(model_bytes: &[u8]) -> Result<Self, BridgeError> {
        let mut handle = std::ptr::null_mut();
        // SAFETY: the model slice remains valid for the call and the output
        // pointer refers to initialized local storage.
        status_result(unsafe {
            kfaceauth_sface_create(model_bytes.as_ptr(), model_bytes.len(), &mut handle)
        })?;
        let handle = NonNull::new(handle).ok_or(BridgeError::RuntimeFailure)?;
        Ok(Self {
            handle,
            _not_send_or_sync: PhantomData,
        })
    }

    #[must_use]
    pub fn backend(&self) -> InferenceBackend {
        InferenceBackend::from_raw(unsafe { kfaceauth_sface_backend(self.handle.as_ptr()) })
    }

    /// Aligns the five YuNet landmarks, crops a 112x112 BGR face, and extracts
    /// the raw 128-element FP32 SFace feature.
    ///
    /// # Errors
    ///
    /// Returns a stable error if geometry, landmarks, native shape, type, or
    /// values violate the reviewed contract.
    pub fn extract(
        &self,
        bgr_bytes: &[u8],
        width: u32,
        height: u32,
        stride: usize,
        detection: &RawDetection,
    ) -> Result<[f32; SFACE_EMBEDDING_DIMENSION], BridgeError> {
        let width = i32::try_from(width).map_err(|_| BridgeError::InvalidArgument)?;
        let height = i32::try_from(height).map_err(|_| BridgeError::InvalidArgument)?;
        let mut embedding = [0.0_f32; SFACE_EMBEDDING_DIMENSION];
        let mut count = 0_usize;
        // SAFETY: all buffers and the opaque handle remain valid for the
        // duration of the call; the output array is uniquely writable.
        status_result(unsafe {
            kfaceauth_sface_extract(
                self.handle.as_ptr(),
                bgr_bytes.as_ptr(),
                bgr_bytes.len(),
                width,
                height,
                stride,
                detection,
                embedding.as_mut_ptr(),
                embedding.len(),
                &mut count,
            )
        })?;
        if count != SFACE_EMBEDDING_DIMENSION {
            embedding.zeroize();
            return Err(BridgeError::MalformedOutput);
        }
        Ok(embedding)
    }

    /// Compares two validated SFace features through OpenCV's cosine
    /// implementation. Matching policy remains owned by safe Rust.
    ///
    /// # Errors
    ///
    /// Returns a stable error for malformed inputs or native output.
    pub fn cosine(
        &self,
        left: &[f32; SFACE_EMBEDDING_DIMENSION],
        right: &[f32; SFACE_EMBEDDING_DIMENSION],
    ) -> Result<f64, BridgeError> {
        let mut similarity = 0.0_f64;
        // SAFETY: input arrays remain valid and immutable for the call and the
        // output pointer refers to initialized local storage.
        status_result(unsafe {
            kfaceauth_sface_cosine(
                self.handle.as_ptr(),
                left.as_ptr(),
                left.len(),
                right.as_ptr(),
                right.len(),
                &mut similarity,
            )
        })?;
        Ok(similarity)
    }
}

impl Drop for Recognizer {
    fn drop(&mut self) {
        // SAFETY: the handle is uniquely owned and destroyed exactly once.
        unsafe { kfaceauth_sface_destroy(self.handle.as_ptr()) };
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HeadPose {
    pub yaw: f32,
    pub pitch: f32,
    pub roll: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TextureMetrics {
    pub lbp_entropy: f32,
    pub moire_energy: f32,
}

/// Estimates 3D head pose (yaw, pitch, roll in degrees) using 5-point canonical anthropometric PnP.
///
/// # Errors
///
/// Returns [`BridgeError`] on invalid input or runtime estimation failure.
pub fn estimate_head_pose(
    detection: &RawDetection,
    image_width: u32,
    image_height: u32,
) -> Result<HeadPose, BridgeError> {
    let width_i32 = i32::try_from(image_width).map_err(|_| BridgeError::InvalidArgument)?;
    let height_i32 = i32::try_from(image_height).map_err(|_| BridgeError::InvalidArgument)?;
    let mut pose = HeadPose::default();
    // SAFETY: pointers are valid and bounds are checked.
    let status =
        unsafe { kfaceauth_estimate_head_pose(detection, width_i32, height_i32, &mut pose) };
    status_result(status)?;
    Ok(pose)
}

/// Analyzes high-frequency facial texture: Local Binary Pattern entropy and 2D FFT moiré energy.
///
/// # Errors
///
/// Returns [`BridgeError`] on invalid geometry or runtime failure.
pub fn analyze_texture(
    bgr_bytes: &[u8],
    width: u32,
    height: u32,
    stride: usize,
    detection: &RawDetection,
) -> Result<TextureMetrics, BridgeError> {
    let width_i32 = i32::try_from(width).map_err(|_| BridgeError::InvalidArgument)?;
    let height_i32 = i32::try_from(height).map_err(|_| BridgeError::InvalidArgument)?;
    let mut metrics = TextureMetrics::default();
    // SAFETY: pointer and slice length are guaranteed valid.
    let status = unsafe {
        kfaceauth_analyze_texture(
            bgr_bytes.as_ptr(),
            bgr_bytes.len(),
            width_i32,
            height_i32,
            stride,
            detection,
            &mut metrics,
        )
    };
    status_result(status)?;
    Ok(metrics)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_invalid_constructor_inputs_before_runtime_use() {
        assert!(matches!(
            Detector::new(&[], 320, 320, 0.9, 0.3, 8),
            Err(BridgeError::InvalidArgument)
        ));
        assert!(matches!(
            Recognizer::new(&[]),
            Err(BridgeError::InvalidArgument)
        ));
    }

    #[test]
    fn reports_an_opencv_version() {
        assert!(!opencv_version().is_empty());
    }

    #[test]
    fn head_pose_and_texture_reject_invalid_inputs() {
        let detection = RawDetection::default();
        assert_eq!(
            estimate_head_pose(&detection, 0, 0),
            Err(BridgeError::InvalidArgument)
        );
        assert_eq!(
            analyze_texture(&[], 0, 0, 0, &detection),
            Err(BridgeError::InvalidArgument)
        );
    }
}
