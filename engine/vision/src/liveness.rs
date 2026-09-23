// SPDX-License-Identifier: GPL-3.0-or-later

//! Experimental, unqualified presentation-attack analysis primitives.
//!
//! These heuristics are not an authentication system and do not constitute
//! ISO/IEC 30107-3 testing or certification. Texture-analysis errors fail closed.

#![forbid(unsafe_code)]

use kfaceauth_vision_opencv_sys::{
    HeadPose, RawDetection, TextureMetrics, analyze_texture, estimate_head_pose,
};

/// Maximum LBP Shannon entropy for bona fide human skin (beyond which printed paper grain/halftone is detected).
pub const MAX_BONA_FIDE_LBP_ENTROPY: f32 = 7.35;

/// Maximum 2D FFT periodic moiré/PAPR energy for bona fide human skin (beyond which display refresh rasters are detected).
pub const MAX_BONA_FIDE_MOIRE_ENERGY: f32 = 0.42;

/// Minimum NIR to visible skin reflectance ratio (human skin ~0.4–0.9; electronic screens emit <0.15 NIR).
pub const MIN_NIR_REFLECTANCE_RATIO: f32 = 0.25;

/// Physiological blink duration bounds in milliseconds (ISO/IEC 30107-3 recommendation).
pub const MIN_BLINK_DURATION_MS: u64 = 100;
pub const MAX_BLINK_DURATION_MS: u64 = 300;

/// Maximum allowable time to satisfy a randomized micro-pose prompt.
pub const PROMPT_DEADLINE_MS: u64 = 1800;

/// Minimum angular deflection threshold in degrees required to confirm head micro-pose challenge.
pub const TARGET_DEFLECTION_DEGREES: f32 = 10.0;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SpoofKind {
    PrintAttack,
    ScreenReplay,
    UnnaturalBlink,
    PoseChallengeFailed,
    LowNirReflectance,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LivenessDecision {
    BonaFide,
    SpoofDetected(SpoofKind),
    AwaitingChallenge(PoseChallenge),
    AnalysisFailed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PoseChallenge {
    TiltLeft,
    TiltRight,
    NodUp,
    NodDown,
    TurnLeft,
    TurnRight,
    Blink,
}

impl PoseChallenge {
    #[must_use]
    pub const fn user_prompt(&self) -> &'static str {
        match self {
            Self::TiltLeft => "Tilt head slightly left",
            Self::TiltRight => "Tilt head slightly right",
            Self::NodUp => "Nod head slightly upward",
            Self::NodDown => "Nod head slightly downward",
            Self::TurnLeft => "Turn head slightly left",
            Self::TurnRight => "Turn head slightly right",
            Self::Blink => "Blink naturally to verify",
        }
    }

    #[must_use]
    pub fn is_satisfied(&self, pose: &HeadPose, blink_completed: bool) -> bool {
        match self {
            Self::TiltLeft => pose.roll >= TARGET_DEFLECTION_DEGREES,
            Self::TiltRight => pose.roll <= -TARGET_DEFLECTION_DEGREES,
            Self::NodUp => pose.pitch >= TARGET_DEFLECTION_DEGREES,
            Self::NodDown => pose.pitch <= -TARGET_DEFLECTION_DEGREES,
            Self::TurnLeft => pose.yaw >= (TARGET_DEFLECTION_DEGREES + 2.0),
            Self::TurnRight => pose.yaw <= -(TARGET_DEFLECTION_DEGREES + 2.0),
            Self::Blink => blink_completed,
        }
    }
}

/// Passive texture and moiré analysis evaluator.
pub struct PassiveTextureAnalyzer;

impl PassiveTextureAnalyzer {
    /// Evaluates passive print attack and screen replay risk from texture metrics.
    #[must_use]
    pub fn evaluate(metrics: &TextureMetrics) -> Option<SpoofKind> {
        if metrics.lbp_entropy > MAX_BONA_FIDE_LBP_ENTROPY {
            return Some(SpoofKind::PrintAttack);
        }
        if metrics.moire_energy > MAX_BONA_FIDE_MOIRE_ENERGY {
            return Some(SpoofKind::ScreenReplay);
        }
        None
    }
}

/// Active physiological eye blink state tracker.
#[derive(Clone, Debug)]
pub struct EyeBlinkTracker {
    last_state: BlinkState,
    state_start_ms: u64,
    closed_duration_ms: u64,
    blink_completed: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum BlinkState {
    Open,
    Closing,
    Closed,
    Opening,
}

impl Default for EyeBlinkTracker {
    fn default() -> Self {
        Self::new()
    }
}

impl EyeBlinkTracker {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            last_state: BlinkState::Open,
            state_start_ms: 0,
            closed_duration_ms: 0,
            blink_completed: false,
        }
    }

    /// Updates eye state given an openness score in [0.0, 1.0] and the frame timestamp.
    pub fn update(&mut self, timestamp_ms: u64, openness: f32) {
        if self.blink_completed {
            return;
        }

        let current_state = if openness <= 0.35 {
            BlinkState::Closed
        } else if openness >= 0.65 {
            BlinkState::Open
        } else if self.last_state == BlinkState::Open || self.last_state == BlinkState::Closing {
            BlinkState::Closing
        } else {
            BlinkState::Opening
        };

        if current_state != self.last_state {
            if self.last_state == BlinkState::Closed {
                self.closed_duration_ms = timestamp_ms.saturating_sub(self.state_start_ms);
                if self.closed_duration_ms >= MIN_BLINK_DURATION_MS
                    && self.closed_duration_ms <= MAX_BLINK_DURATION_MS
                {
                    self.blink_completed = true;
                }
            }
            self.last_state = current_state;
            self.state_start_ms = timestamp_ms;
        }
    }

    #[must_use]
    pub const fn is_completed(&self) -> bool {
        self.blink_completed
    }

    #[must_use]
    pub const fn closed_duration(&self) -> u64 {
        self.closed_duration_ms
    }

    pub fn reset(&mut self) {
        self.last_state = BlinkState::Open;
        self.state_start_ms = 0;
        self.closed_duration_ms = 0;
        self.blink_completed = false;
    }
}

/// Smoothed micro-pose tracker using exponential moving average (EMA) to prevent `PnP` noise.
#[derive(Clone, Debug)]
pub struct SmoothedPoseTracker {
    current_pose: HeadPose,
    initialized: bool,
    alpha: f32, // Smoothing weight (0.0 to 1.0)
}

impl Default for SmoothedPoseTracker {
    fn default() -> Self {
        Self::new(0.65)
    }
}

impl SmoothedPoseTracker {
    #[must_use]
    pub const fn new(alpha: f32) -> Self {
        Self {
            current_pose: HeadPose {
                yaw: 0.0,
                pitch: 0.0,
                roll: 0.0,
            },
            initialized: false,
            alpha,
        }
    }

    pub fn update(&mut self, raw: HeadPose) -> HeadPose {
        if self.initialized {
            self.current_pose.yaw =
                self.alpha * raw.yaw + (1.0 - self.alpha) * self.current_pose.yaw;
            self.current_pose.pitch =
                self.alpha * raw.pitch + (1.0 - self.alpha) * self.current_pose.pitch;
            self.current_pose.roll =
                self.alpha * raw.roll + (1.0 - self.alpha) * self.current_pose.roll;
        } else {
            self.current_pose = raw;
            self.initialized = true;
        }
        self.current_pose
    }

    #[must_use]
    pub const fn pose(&self) -> HeadPose {
        self.current_pose
    }
}

/// Unified Presentation Attack Detection (PAD) Engine.
pub struct PresentationAttackDetector {
    challenge: PoseChallenge,
    pose_tracker: SmoothedPoseTracker,
    blink_tracker: EyeBlinkTracker,
    session_start_ms: Option<u64>,
    challenge_satisfied: bool,
    require_active_challenge: bool,
}

impl PresentationAttackDetector {
    /// Creates a detector with a designated active challenge and configuration.
    #[must_use]
    pub fn new(challenge: PoseChallenge, require_active_challenge: bool) -> Self {
        Self {
            challenge,
            pose_tracker: SmoothedPoseTracker::new(0.65),
            blink_tracker: EyeBlinkTracker::new(),
            session_start_ms: None,
            challenge_satisfied: false,
            require_active_challenge,
        }
    }

    /// Selects a deterministic pseudo-random challenge from seed.
    #[must_use]
    pub fn with_seed(seed: u64, require_active_challenge: bool) -> Self {
        let challenges = [
            PoseChallenge::TurnLeft,
            PoseChallenge::TurnRight,
            PoseChallenge::TiltLeft,
            PoseChallenge::TiltRight,
            PoseChallenge::NodUp,
            PoseChallenge::NodDown,
            PoseChallenge::Blink,
        ];
        #[allow(clippy::cast_possible_truncation)]
        let idx = (seed as usize) % challenges.len();
        Self::new(challenges[idx], require_active_challenge)
    }

    #[must_use]
    pub const fn current_challenge(&self) -> PoseChallenge {
        self.challenge
    }

    /// Evaluates a single static frame against passive attack vectors (LBP texture & 2D FFT moiré).
    #[must_use]
    pub fn evaluate_single_frame(
        bgr_bytes: &[u8],
        width: u32,
        height: u32,
        stride: usize,
        detection: &RawDetection,
        nir_reflectance_ratio: Option<f32>,
    ) -> LivenessDecision {
        // 1. NIR multi-spectrum check (if available)
        if let Some(ratio) = nir_reflectance_ratio {
            if ratio < MIN_NIR_REFLECTANCE_RATIO {
                return LivenessDecision::SpoofDetected(SpoofKind::LowNirReflectance);
            }
        }

        // 2. Passive texture analysis
        match analyze_texture(bgr_bytes, width, height, stride, detection) {
            Ok(metrics) => {
                if let Some(spoof) = PassiveTextureAnalyzer::evaluate(&metrics) {
                    return LivenessDecision::SpoofDetected(spoof);
                }
            }
            Err(_) => return LivenessDecision::AnalysisFailed,
        }

        LivenessDecision::BonaFide
    }

    /// Evaluates a continuous video streaming step against active challenge-response and passive PAD.
    #[allow(clippy::too_many_arguments)]
    pub fn evaluate_stream_step(
        &mut self,
        timestamp_ms: u64,
        bgr_bytes: &[u8],
        width: u32,
        height: u32,
        stride: usize,
        detection: &RawDetection,
        eye_openness: f32,
        nir_reflectance_ratio: Option<f32>,
    ) -> LivenessDecision {
        let session_start = *self.session_start_ms.get_or_insert(timestamp_ms);

        // 1. Passive single-frame checks first (fail-closed immediately on print or screen moiré)
        let passive = Self::evaluate_single_frame(
            bgr_bytes,
            width,
            height,
            stride,
            detection,
            nir_reflectance_ratio,
        );
        if matches!(
            passive,
            LivenessDecision::SpoofDetected(_) | LivenessDecision::AnalysisFailed
        ) {
            return passive;
        }

        if !self.require_active_challenge {
            return LivenessDecision::BonaFide;
        }

        // 2. Active challenge check
        if self.challenge_satisfied {
            return LivenessDecision::BonaFide;
        }

        // Check timeout deadline
        if timestamp_ms.saturating_sub(session_start) > PROMPT_DEADLINE_MS {
            return LivenessDecision::SpoofDetected(SpoofKind::PoseChallengeFailed);
        }

        // Track eye blink
        self.blink_tracker.update(timestamp_ms, eye_openness);

        // Track head pose
        if let Ok(raw_pose) = estimate_head_pose(detection, width, height) {
            let smoothed = self.pose_tracker.update(raw_pose);
            if self
                .challenge
                .is_satisfied(&smoothed, self.blink_tracker.is_completed())
            {
                self.challenge_satisfied = true;
                return LivenessDecision::BonaFide;
            }
        }

        LivenessDecision::AwaitingChallenge(self.challenge)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_detection() -> RawDetection {
        RawDetection {
            values: [
                20.0, 20.0, 72.0, 72.0, 39.0, 44.0, 73.0, 44.0, 56.0, 55.0, 42.0, 74.0, 70.0, 74.0,
                0.95,
            ],
        }
    }

    #[test]
    fn passive_texture_detects_print_attacks() {
        let spoof_metrics = TextureMetrics {
            lbp_entropy: 7.85, // Above threshold of 7.35
            moire_energy: 0.15,
        };
        assert_eq!(
            PassiveTextureAnalyzer::evaluate(&spoof_metrics),
            Some(SpoofKind::PrintAttack)
        );

        let bona_fide_metrics = TextureMetrics {
            lbp_entropy: 6.20,
            moire_energy: 0.18,
        };
        assert_eq!(PassiveTextureAnalyzer::evaluate(&bona_fide_metrics), None);
    }

    #[test]
    fn passive_texture_detects_screen_replay_moire() {
        let screen_metrics = TextureMetrics {
            lbp_entropy: 6.50,
            moire_energy: 0.65, // Above threshold of 0.42
        };
        assert_eq!(
            PassiveTextureAnalyzer::evaluate(&screen_metrics),
            Some(SpoofKind::ScreenReplay)
        );
    }

    #[test]
    fn active_blink_tracker_validates_physiological_profile() {
        let mut tracker = EyeBlinkTracker::new();

        // Eye is open
        tracker.update(0, 0.9);
        assert!(!tracker.is_completed());

        // Eye begins closing
        tracker.update(30, 0.5);
        // Eye is closed
        tracker.update(60, 0.2);
        // Eye stays closed for 150 ms (within 100-300 ms bounds)
        tracker.update(210, 0.2);
        // Eye re-opens
        tracker.update(250, 0.85);

        assert!(tracker.is_completed());
        assert_eq!(tracker.closed_duration(), 190);
    }

    #[test]
    fn active_blink_tracker_rejects_unnatural_blinks() {
        let mut tracker = EyeBlinkTracker::new();

        // Eye is open
        tracker.update(0, 0.9);
        // Eye closes for only 30 ms (too fast, sensor noise)
        tracker.update(20, 0.2);
        tracker.update(50, 0.9);
        assert!(!tracker.is_completed());

        // Eye closes for 600 ms (sleeping or occlusion)
        tracker.reset();
        tracker.update(0, 0.9);
        tracker.update(100, 0.2);
        tracker.update(750, 0.9);
        assert!(!tracker.is_completed());
    }

    #[test]
    fn head_pose_challenge_satisfaction_and_deadline() {
        let mut detector = PresentationAttackDetector::new(PoseChallenge::TurnLeft, true);

        let detection = valid_detection();
        let frame = vec![128_u8; 112 * 112 * 3];

        // Frame at t=0: awaiting challenge
        let decision = detector.evaluate_stream_step(
            0,
            &frame,
            112,
            112,
            112 * 3,
            &detection,
            0.9,
            Some(0.55),
        );
        assert_eq!(
            decision,
            LivenessDecision::AwaitingChallenge(PoseChallenge::TurnLeft)
        );

        // Frame at t=2000 ms (past 1800 ms deadline): fails closed
        let decision = detector.evaluate_stream_step(
            2000,
            &frame,
            112,
            112,
            112 * 3,
            &detection,
            0.9,
            Some(0.55),
        );
        assert_eq!(
            decision,
            LivenessDecision::SpoofDetected(SpoofKind::PoseChallengeFailed)
        );
    }

    #[test]
    fn multi_spectrum_nir_differential_rejects_screen_absorption() {
        let detection = RawDetection::default();
        let frame = vec![128_u8; 112 * 112 * 3];

        // Screen emits 0.08 NIR ratio (screens emit zero NIR) -> LowNirReflectance
        let decision = PresentationAttackDetector::evaluate_single_frame(
            &frame,
            112,
            112,
            112 * 3,
            &detection,
            Some(0.08),
        );
        assert_eq!(
            decision,
            LivenessDecision::SpoofDetected(SpoofKind::LowNirReflectance)
        );

        // Invalid geometry cannot be classified as bona fide, even when NIR looks acceptable.
        let decision = PresentationAttackDetector::evaluate_single_frame(
            &frame,
            112,
            112,
            112 * 3,
            &detection,
            Some(0.65),
        );
        assert_eq!(decision, LivenessDecision::AnalysisFailed);
    }
}
