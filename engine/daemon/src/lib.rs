// SPDX-License-Identifier: GPL-3.0-or-later

//! Standalone System Daemon (`kfaceauthd`) supporting systemd socket activation,
//! caller UID isolation via `SO_PEERCRED`, and least-privilege system vault access.

#![forbid(unsafe_code)]

use std::collections::{HashMap, VecDeque};
use std::fs;
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use zeroize::{Zeroize, Zeroizing};

use kfaceauth_crypto_openssl_sys::{PeerCredentials, peer_credentials};
use kfaceauth_protocol::{read_frame, write_frame};
use kfaceauth_templates::auth_policy::{AuthPolicyMode, AuthPolicySnapshot, AuthTarget};
use kfaceauth_templates::{MasterKey, ProfileSummary, Vault, VaultStatus, VerificationResult};
use kfaceauth_vision::identity::{ExtractionPurpose, IdentityProvider};
use kfaceauth_vision::{
    CancellationToken, ImageView, MAX_FRAME_BYTES, PixelFormat, ProcessingControl,
};

pub const DAEMON_PROTOCOL_VERSION: u16 = 2;
pub const AUTH_POLICY_PATH: &str = "/etc/kfaceauth/policy.json";
pub const DEFAULT_SOCKET_PATH: &str = "/run/kfaceauth/kfaceauthd.sock";
pub const DEFAULT_DAEMON_USER: &str = "kfaceauth";
pub const DEFAULT_DAEMON_GROUP: &str = "kfaceauth";
pub const SOCKET_FILE_MODE: u32 = 0o666;

pub const MAX_DAEMON_REQUEST_BYTES: usize = 14 + 255;
const AUTH_WORKER_PATH: &str = "/usr/libexec/kfaceauth-auth-worker";
const AUTH_WORKER_REQUEST_BYTES: usize = 9;
pub const MAX_DAEMON_RESPONSE_BYTES: usize = 64 * 1024;
pub const DEFAULT_PAM_TIMEOUT_MS: u32 = 2000;
pub const MAX_ACTIVE_CONNECTIONS: usize = 4;
pub const MAX_CONNECTIONS_PER_PEER_UID: usize = 2;
const MIN_REMAINING_TIMEOUT_MS: u32 = 1;
const PAM_RESPONSE_RESERVE: Duration = Duration::from_millis(100);
pub const PAM_ATTEMPT_WINDOW: Duration = Duration::from_secs(30);
pub const PAM_ATTEMPTS_PER_WINDOW: usize = 3;

pub const OP_PAM_AUTH: u8 = 0x10;
pub const OP_STATUS: u8 = 0x11;
pub const OP_FRESHNESS: u8 = 0x12;
pub const FRESHNESS_PROTOCOL_VERSION: u16 = 3;

pub const AUTH_TARGET_SDDM: u8 = 1;
pub const AUTH_TARGET_PLASMA_LOCK: u8 = 2;

pub const STATUS_SUCCESS: u8 = 0x00;
pub const STATUS_AUTH_FAILED: u8 = 0x01;
pub const STATUS_ACCESS_DENIED: u8 = 0x02;
pub const STATUS_NO_PROFILE: u8 = 0x03;
pub const STATUS_TIMEOUT: u8 = 0x04;
pub const STATUS_DEVICE_BUSY: u8 = 0x05;
pub const STATUS_INTERNAL_ERROR: u8 = 0x06;
pub const STATUS_SPOOF_DETECTED: u8 = 0x07;
pub const STATUS_RATE_LIMITED: u8 = 0x08;
pub const STATUS_CANCELLED: u8 = 0x09;
pub const STATUS_PROGRESS_LOOKING_FOR_FACE: u8 = 0x80;

#[derive(Debug, Default)]
pub struct AuthRateLimiter {
    attempts: Mutex<HashMap<u32, VecDeque<Instant>>>,
}

impl AuthRateLimiter {
    fn allow_at(&self, uid: u32, now: Instant) -> bool {
        let Ok(mut attempts) = self.attempts.lock() else {
            return false;
        };
        attempts.retain(|_, times| {
            times.retain(|at| now.saturating_duration_since(*at) < PAM_ATTEMPT_WINDOW);
            !times.is_empty()
        });
        let times = attempts.entry(uid).or_default();
        if times.len() >= PAM_ATTEMPTS_PER_WINDOW {
            return false;
        }
        times.push_back(now);
        true
    }

    fn allow(&self, uid: u32) -> bool {
        self.allow_at(uid, Instant::now())
    }

    fn clear(&self, uid: u32) {
        if let Ok(mut attempts) = self.attempts.lock() {
            attempts.remove(&uid);
        }
    }
}

#[derive(Clone, Debug)]
pub struct DaemonConfig {
    pub model_root: PathBuf,
    pub keys_dir: Option<PathBuf>,
    pub vault_root: Option<PathBuf>,
    pub camera_device: Option<String>,
    pub test_frame_path: Option<PathBuf>,
    pub max_timeout_ms: u32,
    rate_limiter: Arc<AuthRateLimiter>,
    auth_child: Arc<Mutex<Option<Child>>>,
    #[cfg(test)]
    worker_path: Option<PathBuf>,
    #[cfg(test)]
    policy_directory: Option<Arc<fs::File>>,
    #[cfg(test)]
    synthetic_policy: bool,
}

impl Default for DaemonConfig {
    fn default() -> Self {
        Self::from_env()
    }
}

impl DaemonConfig {
    #[must_use]
    pub fn from_env() -> Self {
        let model_root = std::env::var_os("KFACEAUTH_MODEL_DIR").map_or_else(
            || PathBuf::from("/usr/share/kfaceauth/models"),
            PathBuf::from,
        );
        let keys_dir = std::env::var_os("KFACEAUTH_KEYS_DIR").map(PathBuf::from);
        let vault_root = std::env::var_os("KFACEAUTH_SYSTEM_VAULT_DIR").map(PathBuf::from);
        let camera_device = std::env::var("KFACEAUTH_CAMERA_DEVICE").ok();
        let test_frame_path = std::env::var_os("KFACEAUTH_TEST_FRAME").map(PathBuf::from);
        Self {
            model_root,
            keys_dir,
            vault_root,
            camera_device,
            test_frame_path,
            max_timeout_ms: DEFAULT_PAM_TIMEOUT_MS,
            rate_limiter: Arc::new(AuthRateLimiter::default()),
            auth_child: Arc::new(Mutex::new(None)),
            #[cfg(test)]
            worker_path: None,
            #[cfg(test)]
            policy_directory: None,
            #[cfg(test)]
            synthetic_policy: false,
        }
    }
}

/// Binds requests to the kernel-authenticated client identity. Root (UID 0)
/// callers (such as SDDM helper and system PAM services) are authorized to
/// authenticate on behalf of any valid system target UID. Unprivileged callers
/// are strictly restricted to their own UID.
#[must_use]
pub const fn is_authorized(peer_uid: u32, target_uid: u32) -> bool {
    peer_uid == target_uid || peer_uid == 0
}

#[derive(Debug)]
pub enum DaemonRequest {
    PamAuth {
        target_uid: u32,
        auth_target: u8,
        timeout_ms: u32,
        username: String,
    },
    Status {
        target_uid: u32,
    },
    Freshness {
        target_uid: u32,
        local_digest: [u8; 32],
    },
}

impl DaemonRequest {
    #[must_use]
    pub const fn target_uid(&self) -> u32 {
        match self {
            Self::PamAuth { target_uid, .. }
            | Self::Status { target_uid }
            | Self::Freshness { target_uid, .. } => *target_uid,
        }
    }
}

/// Decodes one incoming daemon request frame.
///
/// # Errors
///
/// Returns an error string if payload length, version, or format is invalid.
pub fn decode_daemon_request(payload: &[u8]) -> Result<DaemonRequest, &'static str> {
    if payload.len() < 8 || payload.len() > MAX_DAEMON_REQUEST_BYTES {
        return Err("invalid request payload length");
    }
    let version = u16::from_be_bytes([payload[0], payload[1]]);
    if version == FRESHNESS_PROTOCOL_VERSION
        && payload.len() == 40
        && payload[2] == OP_FRESHNESS
        && payload[3] == 0
    {
        return Ok(DaemonRequest::Freshness {
            target_uid: u32::from_be_bytes(payload[4..8].try_into().map_err(|_| "invalid UID")?),
            local_digest: payload[8..40].try_into().map_err(|_| "invalid digest")?,
        });
    }
    if version != DAEMON_PROTOCOL_VERSION {
        return Err("unsupported protocol version");
    }
    let opcode = payload[2];
    let target_uid = u32::from_be_bytes([payload[4], payload[5], payload[6], payload[7]]);

    match opcode {
        OP_PAM_AUTH => {
            if payload.len() < 14 {
                return Err("malformed PAM request payload");
            }
            let auth_target = payload[3];
            if !matches!(auth_target, AUTH_TARGET_SDDM | AUTH_TARGET_PLASMA_LOCK) {
                return Err("invalid authentication target");
            }
            let timeout_ms = u32::from_be_bytes([payload[8], payload[9], payload[10], payload[11]]);
            let user_len = usize::from(u16::from_be_bytes([payload[12], payload[13]]));
            if !(1..=DEFAULT_PAM_TIMEOUT_MS).contains(&timeout_ms)
                || !(1..=255).contains(&user_len)
                || payload.len() != 14 + user_len
            {
                return Err("invalid PAM request bounds");
            }
            let username =
                std::str::from_utf8(&payload[14..]).map_err(|_| "invalid username encoding")?;
            if username.contains('\0') {
                return Err("invalid username");
            }
            let username = username.to_owned();
            Ok(DaemonRequest::PamAuth {
                target_uid,
                auth_target,
                timeout_ms,
                username,
            })
        }
        OP_STATUS if payload.len() == 8 && payload[3] == 0 => {
            Ok(DaemonRequest::Status { target_uid })
        }
        _ => Err("unknown opcode"),
    }
}

/// Encodes one daemon response frame payload.
#[must_use]
pub fn encode_daemon_response(status_code: u8, extra: &[u8]) -> Vec<u8> {
    let version = DAEMON_PROTOCOL_VERSION.to_be_bytes();
    let mut response = Vec::with_capacity(4 + extra.len());
    response.push(version[0]);
    response.push(version[1]);
    response.push(status_code);
    response.push(0);
    response.extend_from_slice(extra);
    response
}

fn open_vault_for_uid(target_uid: u32, config: &DaemonConfig) -> Result<Vault, u8> {
    match &config.vault_root {
        Some(root) => Ok(Vault::system_with_root(root, target_uid)),
        None => Vault::system(target_uid).map_err(|_| STATUS_INTERNAL_ERROR),
    }
}

fn authentication_target(auth_target: u8) -> Option<AuthTarget> {
    match auth_target {
        AUTH_TARGET_SDDM => Some(AuthTarget::Sddm),
        AUTH_TARGET_PLASMA_LOCK => Some(AuthTarget::PlasmaLock),
        _ => None,
    }
}

enum AttemptPolicy {
    Verified(AuthPolicySnapshot),
    #[cfg(test)]
    Synthetic,
}

impl AttemptPolicy {
    fn mode_for(&self, uid: u32, target: AuthTarget) -> AuthPolicyMode {
        match self {
            Self::Verified(snapshot) => snapshot.policy().mode_for(uid, target),
            #[cfg(test)]
            Self::Synthetic => AuthPolicyMode::OnActivity,
        }
    }

    fn is_current(&self, current: &Self) -> bool {
        match (self, current) {
            (Self::Verified(start), Self::Verified(end)) => start.same_file_as(end),
            #[cfg(test)]
            (Self::Synthetic, Self::Synthetic) => true,
            #[cfg(test)]
            _ => false,
        }
    }
}

fn authentication_policy(config: &DaemonConfig) -> Option<AttemptPolicy> {
    #[cfg(test)]
    if let Some(directory) = &config.policy_directory {
        return AuthPolicySnapshot::load_from_directory(directory)
            .ok()
            .map(AttemptPolicy::Verified);
    }
    #[cfg(test)]
    if config.synthetic_policy {
        return Some(AttemptPolicy::Synthetic);
    }
    #[cfg(not(test))]
    let _ = config;
    AuthPolicySnapshot::load_system()
        .ok()
        .map(AttemptPolicy::Verified)
}

fn authentication_target_enabled(target_uid: u32, auth_target: u8, config: &DaemonConfig) -> bool {
    authentication_target(auth_target)
        .zip(authentication_policy(config))
        .is_some_and(|(target, policy)| policy.mode_for(target_uid, target) != AuthPolicyMode::Off)
}

type CapturedFrame = (u32, u32, u32, u8, Zeroizing<Vec<u8>>);

fn load_test_frame(path: &Path) -> Result<CapturedFrame, u8> {
    let mut data = Zeroizing::new(Vec::new());
    fs::File::open(path)
        .and_then(|file| {
            file.take((MAX_FRAME_BYTES + 14) as u64)
                .read_to_end(&mut data)
        })
        .map_err(|_| STATUS_DEVICE_BUSY)?;
    if data.len() > MAX_FRAME_BYTES + 13 {
        return Err(STATUS_DEVICE_BUSY);
    }
    if data.len() < 13 {
        return Err(STATUS_DEVICE_BUSY);
    }
    let width = u32::from_be_bytes([data[0], data[1], data[2], data[3]]);
    let height = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
    let stride = u32::from_be_bytes([data[8], data[9], data[10], data[11]]);
    let format = data[12];
    let frame_bytes = Zeroizing::new(data[13..].to_vec());
    Ok((width, height, stride, format, frame_bytes))
}

fn capture_camera_frame(device_path: Option<&str>, timeout_ms: u32) -> Result<CapturedFrame, u8> {
    let mut buffer = Zeroizing::new(vec![0_u8; MAX_FRAME_BYTES]);
    let (width, height, format) =
        kfaceauth_crypto_openssl_sys::v4l2_capture(device_path, timeout_ms, &mut buffer)
            .map_err(|_| STATUS_DEVICE_BUSY)?;
    let format_u8 = u8::try_from(format).map_err(|_| STATUS_DEVICE_BUSY)?;
    let bpp = match format_u8 {
        3 => 1, // PixelFormat::Gray8
        2 => 4, // PixelFormat::Rgba8
        _ => 3, // PixelFormat::Rgb8
    };
    let stride = width.checked_mul(bpp).ok_or(STATUS_DEVICE_BUSY)?;
    let expected_len = usize::try_from(stride.checked_mul(height).ok_or(STATUS_DEVICE_BUSY)?)
        .map_err(|_| STATUS_DEVICE_BUSY)?;
    if buffer.len() < expected_len {
        return Err(STATUS_DEVICE_BUSY);
    }
    buffer[expected_len..].zeroize();
    buffer.truncate(expected_len);
    Ok((width, height, stride, format_u8, buffer))
}

fn remaining_timeout_ms(deadline: Instant) -> Option<u32> {
    let remaining = deadline.checked_duration_since(Instant::now())?;
    if remaining.is_zero() {
        return None;
    }
    let milliseconds = remaining
        .as_millis()
        .max(u128::from(MIN_REMAINING_TIMEOUT_MS));
    Some(u32::try_from(milliseconds.min(u128::from(u32::MAX))).unwrap_or(u32::MAX))
}

#[allow(clippy::too_many_arguments)]
fn verify_frame_against_vault(
    vault: &Vault,
    key: &MasterKey,
    width: u32,
    height: u32,
    stride: u32,
    format_byte: u8,
    frame_bytes: &[u8],
    deadline: Instant,
    model_root: &Path,
) -> u8 {
    let Ok(format) = PixelFormat::try_from(format_byte) else {
        return STATUS_AUTH_FAILED;
    };
    let Ok(image) = ImageView::new(format, width, height, stride, frame_bytes) else {
        return STATUS_AUTH_FAILED;
    };
    if remaining_timeout_ms(deadline).is_none() {
        return STATUS_TIMEOUT;
    }
    let Ok(provider) = IdentityProvider::from_model_root(model_root) else {
        return STATUS_INTERNAL_ERROR;
    };
    let Some(timeout_ms) = remaining_timeout_ms(deadline) else {
        return STATUS_TIMEOUT;
    };
    let cancellation = CancellationToken::default();
    let Ok(control) = ProcessingControl::with_timeout(
        &cancellation,
        Duration::from_millis(u64::from(timeout_ms)),
    ) else {
        return STATUS_TIMEOUT;
    };
    let embedding = match provider.extract(image, control, ExtractionPurpose::ExperimentalAuth) {
        Ok(emb) => emb,
        Err(kfaceauth_vision::identity::IdentityError::SpoofDetected(_)) => {
            return STATUS_SPOOF_DETECTED;
        }
        Err(_) => return STATUS_AUTH_FAILED,
    };
    if remaining_timeout_ms(deadline).is_none() {
        return STATUS_TIMEOUT;
    }
    match vault.open_profile(key) {
        Ok(profile) => {
            if remaining_timeout_ms(deadline).is_none() {
                return STATUS_TIMEOUT;
            }
            match profile.verify(&embedding) {
                VerificationResult::Match => STATUS_SUCCESS,
                VerificationResult::Ambiguous | VerificationResult::NoMatch => STATUS_AUTH_FAILED,
            }
        }
        Err(_) => STATUS_AUTH_FAILED,
    }
}

fn handle_pam_request(
    target_uid: u32,
    auth_target: u8,
    timeout_ms: u32,
    connection_deadline: Instant,
    config: &DaemonConfig,
    mut looking_for_face: impl FnMut() -> bool,
) -> (u8, Vec<u8>) {
    let timeout_ms = timeout_ms
        .min(config.max_timeout_ms)
        .min(DEFAULT_PAM_TIMEOUT_MS);
    let request_deadline = Instant::now()
        .checked_add(Duration::from_millis(u64::from(timeout_ms)))
        .map_or(connection_deadline, |requested| {
            requested.min(connection_deadline)
        });
    if remaining_timeout_ms(request_deadline).is_none() {
        return (STATUS_TIMEOUT, Vec::new());
    }
    if !authentication_target_enabled(target_uid, auth_target, config) {
        return (STATUS_ACCESS_DENIED, Vec::new());
    }
    let keys_dir = config.keys_dir.as_deref();

    let Ok(mut key_bytes) =
        kfaceauth_crypto_openssl_sys::load_master_key_for_uid(target_uid, keys_dir)
    else {
        return (STATUS_AUTH_FAILED, Vec::new());
    };
    let master_key = MasterKey::from_bytes(key_bytes);
    key_bytes.zeroize();
    if remaining_timeout_ms(request_deadline).is_none() {
        return (STATUS_TIMEOUT, Vec::new());
    }

    let vault = match open_vault_for_uid(target_uid, config) {
        Ok(v) => v,
        Err(status) => return (status, Vec::new()),
    };

    match vault.status(Some(&master_key)) {
        VaultStatus::Ready(ProfileSummary {
            enrolled: true,
            sample_count,
        }) if sample_count > 0 => {}
        _ => return (STATUS_NO_PROFILE, Vec::new()),
    }
    let Some(capture_timeout_ms) = remaining_timeout_ms(request_deadline) else {
        return (STATUS_TIMEOUT, Vec::new());
    };

    let frame_result = if let Some(path) = &config.test_frame_path {
        load_test_frame(path)
    } else {
        capture_camera_frame(config.camera_device.as_deref(), capture_timeout_ms)
    };

    let (width, height, stride, format, frame_bytes) = match frame_result {
        Ok(frame) => frame,
        Err(status) => return (status, Vec::new()),
    };
    if !looking_for_face() {
        return (STATUS_INTERNAL_ERROR, Vec::new());
    }
    if remaining_timeout_ms(request_deadline).is_none() {
        return (STATUS_TIMEOUT, Vec::new());
    }

    let result = verify_frame_against_vault(
        &vault,
        &master_key,
        width,
        height,
        stride,
        format,
        &frame_bytes,
        request_deadline,
        &config.model_root,
    );
    if remaining_timeout_ms(request_deadline).is_none() {
        (STATUS_TIMEOUT, Vec::new())
    } else {
        (result, Vec::new())
    }
}

/// Dispatches a validated daemon request, strictly enforcing peer authorization.
#[must_use]
pub fn dispatch_request(
    request: &DaemonRequest,
    peer: &PeerCredentials,
    config: &DaemonConfig,
) -> Vec<u8> {
    let timeout =
        Duration::from_millis(u64::from(config.max_timeout_ms.min(DEFAULT_PAM_TIMEOUT_MS)));
    let deadline = Instant::now()
        .checked_add(timeout)
        .unwrap_or_else(Instant::now);
    dispatch_request_until(request, peer, config, deadline)
}

fn dispatch_request_until(
    request: &DaemonRequest,
    peer: &PeerCredentials,
    config: &DaemonConfig,
    deadline: Instant,
) -> Vec<u8> {
    dispatch_request_until_with_client(request, peer, config, deadline, None)
}

fn dispatch_request_until_with_client(
    request: &DaemonRequest,
    peer: &PeerCredentials,
    config: &DaemonConfig,
    deadline: Instant,
    cancel_client: Option<&UnixStream>,
) -> Vec<u8> {
    if let DaemonRequest::Freshness {
        target_uid,
        local_digest,
    } = request
    {
        // The informational operation is caller-bound, including root callers.
        let freshness = if peer.uid == *target_uid && remaining_timeout_ms(deadline).is_some() {
            let root = config
                .vault_root
                .clone()
                .unwrap_or_else(kfaceauth_templates::system_vault_root);
            kfaceauth_templates::freshness::profile_freshness(&root, *target_uid, local_digest)
        } else {
            kfaceauth_templates::freshness::ProfileFreshness::Unknown
        };
        return vec![
            0,
            3,
            if peer.uid == *target_uid {
                STATUS_SUCCESS
            } else {
                STATUS_ACCESS_DENIED
            },
            0,
            freshness as u8,
        ];
    }
    let target_uid = request.target_uid();
    // Deny requests that attempt to target a UID other than the socket peer.
    if !is_authorized(peer.uid, target_uid) {
        return encode_daemon_response(STATUS_ACCESS_DENIED, &[]);
    }

    let (status, extra) = match request {
        DaemonRequest::PamAuth {
            target_uid,
            auth_target,
            timeout_ms,
            ..
        } => {
            let Some(start_policy) = authentication_policy(config).filter(|policy| {
                authentication_target(*auth_target).is_some_and(|target| {
                    policy.mode_for(*target_uid, target) != AuthPolicyMode::Off
                })
            }) else {
                return encode_daemon_response(STATUS_ACCESS_DENIED, &[]);
            };
            let timeout_ms = (*timeout_ms)
                .min(config.max_timeout_ms)
                .min(DEFAULT_PAM_TIMEOUT_MS);
            let requested_deadline = Instant::now()
                .checked_add(Duration::from_millis(u64::from(timeout_ms)))
                .map_or(deadline, |requested| requested.min(deadline));
            let Some(processing_deadline) = requested_deadline.checked_sub(PAM_RESPONSE_RESERVE)
            else {
                return encode_daemon_response(STATUS_TIMEOUT, &[]);
            };
            let status = supervise_auth_worker(
                *target_uid,
                *auth_target,
                timeout_ms,
                processing_deadline,
                config,
                cancel_client,
            );
            // Compare the attempt-scoped descriptor identity after the worker
            // exits. A replacement invalidates the attempt even if rollback
            // restores identical policy contents and mode.
            if status == STATUS_SUCCESS
                && !authentication_policy(config)
                    .is_some_and(|current| start_policy.is_current(&current))
            {
                return encode_daemon_response(STATUS_ACCESS_DENIED, &[]);
            }
            if status == STATUS_SUCCESS {
                config.rate_limiter.clear(*target_uid);
            }
            (status, Vec::new())
        }
        DaemonRequest::Freshness { .. } => unreachable!("freshness handled separately"),
        DaemonRequest::Status { target_uid } => {
            let keys_dir = config.keys_dir.as_deref();
            match kfaceauth_crypto_openssl_sys::load_master_key_for_uid(*target_uid, keys_dir) {
                Ok(mut k) => {
                    let master_key = MasterKey::from_bytes(k);
                    k.zeroize();
                    match open_vault_for_uid(*target_uid, config) {
                        Ok(vault) => match vault.status(Some(&master_key)) {
                            VaultStatus::Ready(summary) => (
                                STATUS_SUCCESS,
                                vec![u8::from(summary.enrolled), summary.sample_count],
                            ),
                            VaultStatus::Absent => (STATUS_SUCCESS, vec![0, 0]),
                            _ => (STATUS_NO_PROFILE, vec![0, 0]),
                        },
                        Err(code) => (code, Vec::new()),
                    }
                }
                Err(_) => (STATUS_INTERNAL_ERROR, Vec::new()),
            }
        }
    };

    encode_daemon_response(status, &extra)
}

/// Runs one private worker request. The installed worker accepts no argv data.
///
/// # Errors
/// Returns an I/O error for malformed private input or failed output.
pub fn serve_auth_worker<R: Read, W: Write>(reader: &mut R, writer: &mut W) -> io::Result<()> {
    let mut input = Zeroizing::new([0_u8; AUTH_WORKER_REQUEST_BYTES]);
    reader.read_exact(input.as_mut())?;
    let mut trailing = [0_u8; 1];
    if reader.read(&mut trailing)? != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid private request",
        ));
    }
    let uid = u32::from_be_bytes([input[0], input[1], input[2], input[3]]);
    let timeout_ms = u32::from_be_bytes([input[4], input[5], input[6], input[7]]);
    let auth_target = input[8];
    input.zeroize();
    if !(1..=DEFAULT_PAM_TIMEOUT_MS).contains(&timeout_ms)
        || !matches!(auth_target, AUTH_TARGET_SDDM | AUTH_TARGET_PLASMA_LOCK)
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid private deadline",
        ));
    }
    let deadline = Instant::now() + Duration::from_millis(u64::from(timeout_ms));
    let (status, _) = handle_pam_request(
        uid,
        auth_target,
        timeout_ms,
        deadline,
        &DaemonConfig::from_env(),
        || {
            writer
                .write_all(&encode_daemon_response(
                    STATUS_PROGRESS_LOOKING_FOR_FACE,
                    &[],
                ))
                .is_ok()
        },
    );
    writer.write_all(&encode_daemon_response(status, &[]))
}

fn reap_auth_child(config: &DaemonConfig) {
    if let Ok(mut slot) = config.auth_child.try_lock()
        && let Some(child) = slot.as_mut()
        && matches!(child.try_wait(), Ok(Some(_)))
    {
        *slot = None;
    }
}

fn spawn_auth_worker(config: &DaemonConfig) -> io::Result<(Child, UnixStream)> {
    #[cfg(test)]
    let worker_path = config
        .worker_path
        .as_deref()
        .unwrap_or(Path::new(AUTH_WORKER_PATH));
    #[cfg(not(test))]
    let worker_path = Path::new(AUTH_WORKER_PATH);
    let (response_socket, worker_output) = UnixStream::pair()?;
    let worker_output: std::os::fd::OwnedFd = worker_output.into();
    let mut command = Command::new(worker_path);
    command
        .stdin(Stdio::piped())
        .stdout(Stdio::from(worker_output))
        .stderr(Stdio::null());
    command
        .env_clear()
        .env("KFACEAUTH_MODEL_DIR", &config.model_root);
    if let Some(path) = &config.keys_dir {
        command.env("KFACEAUTH_KEYS_DIR", path);
    }
    if let Some(path) = &config.vault_root {
        command.env("KFACEAUTH_SYSTEM_VAULT_DIR", path);
    }
    if let Some(device) = &config.camera_device {
        command.env("KFACEAUTH_CAMERA_DEVICE", device);
    }
    if let Some(path) = &config.test_frame_path {
        command.env("KFACEAUTH_TEST_FRAME", path);
    }
    let child = command.spawn()?;
    drop(command);
    Ok((child, response_socket))
}

#[allow(clippy::too_many_lines)]
fn supervise_auth_worker(
    uid: u32,
    auth_target: u8,
    timeout_ms: u32,
    deadline: Instant,
    config: &DaemonConfig,
    cancel_client: Option<&UnixStream>,
) -> u8 {
    let Ok(mut slot) = config.auth_child.try_lock() else {
        return STATUS_DEVICE_BUSY;
    };
    if let Some(child) = slot.as_mut() {
        match child.try_wait() {
            Ok(Some(_)) => *slot = None,
            _ => return STATUS_DEVICE_BUSY,
        }
    }
    if remaining_timeout_ms(deadline).is_none() {
        return STATUS_TIMEOUT;
    }
    if !config.rate_limiter.allow(uid) {
        return STATUS_RATE_LIMITED;
    }
    let Ok((mut child, mut response_socket)) = spawn_auth_worker(config) else {
        return STATUS_INTERNAL_ERROR;
    };
    let mut request = Zeroizing::new(Vec::with_capacity(AUTH_WORKER_REQUEST_BYTES));
    request.extend_from_slice(&uid.to_be_bytes());
    request.extend_from_slice(&timeout_ms.to_be_bytes());
    request.push(auth_target);
    let write_result = child.stdin.take().map_or_else(
        || Err(io::Error::other("worker input missing")),
        |mut input| input.write_all(&request),
    );
    *slot = Some(child);
    if write_result.is_err() {
        let _ = slot.as_mut().expect("child registered").kill();
        return STATUS_INTERNAL_ERROR;
    }
    if response_socket.set_nonblocking(true).is_err() {
        let _ = slot.as_mut().expect("child registered").kill();
        return STATUS_INTERNAL_ERROR;
    }
    let mut worker_response = Vec::with_capacity(8);
    let mut final_status = None;
    loop {
        let child = slot.as_mut().expect("child registered");
        if cancel_client.is_some_and(client_disconnected) {
            let _ = child.kill();
            return STATUS_CANCELLED;
        }
        if remaining_timeout_ms(deadline).is_none() {
            let _ = child.kill();
            // Keep ownership until the accept-loop's nonblocking reap succeeds.
            // In particular, SIGKILL cannot force immediate exit from kernel D-state.
            return STATUS_TIMEOUT;
        }
        let mut chunk = [0_u8; 32];
        loop {
            match response_socket.read(&mut chunk) {
                Ok(0) => break,
                Ok(count) => worker_response.extend_from_slice(&chunk[..count]),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => {
                    let _ = child.kill();
                    return STATUS_INTERNAL_ERROR;
                }
            }
        }
        while worker_response.len() >= 4 {
            let frame: Vec<u8> = worker_response.drain(..4).collect();
            if frame[..2] != DAEMON_PROTOCOL_VERSION.to_be_bytes() || frame[3] != 0 {
                let _ = child.kill();
                return STATUS_INTERNAL_ERROR;
            }
            let status = frame[2];
            if status == STATUS_PROGRESS_LOOKING_FOR_FACE {
                if final_status.is_some() {
                    let _ = child.kill();
                    return STATUS_INTERNAL_ERROR;
                }
                if let Some(client) = cancel_client
                    && write_client_progress(client, &frame, deadline).is_err()
                {
                    let _ = child.kill();
                    return STATUS_CANCELLED;
                }
            } else if status <= STATUS_CANCELLED && final_status.is_none() {
                final_status = Some(status);
            } else {
                let _ = child.kill();
                return STATUS_INTERNAL_ERROR;
            }
        }
        match child.try_wait() {
            Ok(Some(exit)) => {
                *slot = None;
                if cancel_client.is_some_and(client_disconnected) {
                    return STATUS_CANCELLED;
                }
                if !exit.success() || !worker_response.is_empty() {
                    return STATUS_INTERNAL_ERROR;
                }
                if remaining_timeout_ms(deadline).is_none() {
                    return STATUS_TIMEOUT;
                }
                let Some(status) = final_status else {
                    return STATUS_INTERNAL_ERROR;
                };
                return status;
            }
            Ok(None) => thread::sleep(Duration::from_millis(2)),
            Err(_) => {
                let _ = child.kill();
                return STATUS_INTERNAL_ERROR;
            }
        }
    }
}

fn write_client_progress(stream: &UnixStream, payload: &[u8], deadline: Instant) -> io::Result<()> {
    let length = u32::try_from(payload.len())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "progress frame too large"))?;
    let mut frame = Vec::with_capacity(4 + payload.len());
    frame.extend_from_slice(&length.to_be_bytes());
    frame.extend_from_slice(payload);
    let mut sent = 0;
    while sent < frame.len() {
        if remaining_timeout_ms(deadline).is_none() {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "progress deadline expired",
            ));
        }
        match (&*stream).write(&frame[sent..]) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "progress write failed",
                ));
            }
            Ok(count) => sent += count,
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(1));
            }
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

fn remaining_duration(deadline: Instant) -> io::Result<Duration> {
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .filter(|duration| !duration.is_zero())
        .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "request deadline expired"))?;
    Ok(remaining)
}

struct DeadlineStream<'a> {
    stream: &'a mut UnixStream,
    deadline: Instant,
}

impl Read for DeadlineStream<'_> {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        self.stream
            .set_read_timeout(Some(remaining_duration(self.deadline)?))?;
        self.stream.read(buffer)
    }
}

impl Write for DeadlineStream<'_> {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        self.stream
            .set_write_timeout(Some(remaining_duration(self.deadline)?))?;
        self.stream.write(buffer)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.stream
            .set_write_timeout(Some(remaining_duration(self.deadline)?))?;
        self.stream.flush()
    }
}

fn connection_deadline(config: &DaemonConfig) -> Instant {
    let timeout = config.max_timeout_ms.min(DEFAULT_PAM_TIMEOUT_MS);
    Instant::now()
        .checked_add(Duration::from_millis(u64::from(timeout)))
        .unwrap_or_else(Instant::now)
}

/// Handles a single incoming client connection on the Unix domain stream socket.
///
/// # Errors
///
/// Returns [`io::Error`] on network/stream I/O failures.
pub fn handle_client_stream(stream: UnixStream, config: &DaemonConfig) -> io::Result<()> {
    let peer = peer_credentials(stream.as_raw_fd()).map_err(|error| {
        io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("peer credentials failed: {error:?}"),
        )
    })?;
    handle_client_stream_with_peer(stream, peer, config, connection_deadline(config))
}

fn handle_client_stream_with_peer(
    mut stream: UnixStream,
    peer: PeerCredentials,
    config: &DaemonConfig,
    deadline: Instant,
) -> io::Result<()> {
    let payload = {
        let mut deadline_stream = DeadlineStream {
            stream: &mut stream,
            deadline,
        };
        read_frame(&mut deadline_stream, MAX_DAEMON_REQUEST_BYTES).map_err(|error| {
            io::Error::new(io::ErrorKind::InvalidData, format!("frame error: {error}"))
        })?
    };

    let request = decode_daemon_request(&payload);
    let monitor_disconnect = matches!(&request, Ok(DaemonRequest::PamAuth { .. }));
    if monitor_disconnect {
        stream.set_nonblocking(true)?;
    }
    let response = match request {
        Ok(request) => dispatch_request_until_with_client(
            &request,
            &peer,
            config,
            deadline,
            monitor_disconnect.then_some(&stream),
        ),
        Err(_) => encode_daemon_response(STATUS_INTERNAL_ERROR, &[]),
    };
    if monitor_disconnect {
        stream.set_nonblocking(false)?;
    }

    let mut deadline_stream = DeadlineStream {
        stream: &mut stream,
        deadline,
    };
    write_frame(&mut deadline_stream, &response, MAX_DAEMON_RESPONSE_BYTES).map_err(|error| {
        io::Error::new(
            io::ErrorKind::TimedOut,
            format!("response write failed before deadline: {error}"),
        )
    })
}

fn client_disconnected(stream: &UnixStream) -> bool {
    let mut probe = [0_u8; 1];
    !matches!(
        (&*stream).read(&mut probe),
        Err(error)
            if matches!(
                error.kind(),
                io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
            )
    )
}

#[derive(Default)]
struct IngressState {
    active_by_uid: Mutex<HashMap<u32, usize>>,
}

impl IngressState {
    fn try_acquire(self: &Arc<Self>, peer_uid: u32) -> Option<IngressPermit> {
        let mut active = self.active_by_uid.lock().ok()?;
        let total: usize = active.values().sum();
        let peer_count = active.get(&peer_uid).copied().unwrap_or_default();
        if total >= MAX_ACTIVE_CONNECTIONS || peer_count >= MAX_CONNECTIONS_PER_PEER_UID {
            return None;
        }
        active.insert(peer_uid, peer_count + 1);
        Some(IngressPermit {
            state: Arc::clone(self),
            peer_uid,
        })
    }
}

struct IngressPermit {
    state: Arc<IngressState>,
    peer_uid: u32,
}

impl Drop for IngressPermit {
    fn drop(&mut self) {
        if let Ok(mut active) = self.state.active_by_uid.lock() {
            if let Some(count) = active.get_mut(&self.peer_uid) {
                *count -= 1;
                if *count == 0 {
                    active.remove(&self.peer_uid);
                }
            }
        }
    }
}

struct DaemonJob {
    stream: UnixStream,
    peer: PeerCredentials,
    deadline: Instant,
    permit: IngressPermit,
}

fn spawn_workers(
    config: &DaemonConfig,
) -> io::Result<(SyncSender<DaemonJob>, Vec<thread::JoinHandle<()>>)> {
    let (sender, receiver) = mpsc::sync_channel::<DaemonJob>(MAX_ACTIVE_CONNECTIONS);
    let receiver = Arc::new(Mutex::new(receiver));
    let config = Arc::new(config.clone());
    let mut workers = Vec::with_capacity(MAX_ACTIVE_CONNECTIONS);
    for index in 0..MAX_ACTIVE_CONNECTIONS {
        let receiver = Arc::clone(&receiver);
        let config = Arc::clone(&config);
        let worker = thread::Builder::new()
            .name(format!("kfaceauth-ingress-{index}"))
            .spawn(move || worker_loop(&receiver, &config))?;
        workers.push(worker);
    }
    Ok((sender, workers))
}

fn worker_loop(receiver: &Arc<Mutex<Receiver<DaemonJob>>>, config: &Arc<DaemonConfig>) {
    loop {
        let next_job = {
            let Ok(receiver) = receiver.lock() else {
                return;
            };
            receiver.recv()
        };
        let Ok(job) = next_job else {
            return;
        };
        let DaemonJob {
            stream,
            peer,
            deadline,
            permit: _permit,
        } = job;
        let _ = handle_client_stream_with_peer(stream, peer, config, deadline);
    }
}

fn reject_busy(mut stream: UnixStream) {
    let _ = stream.set_write_timeout(Some(Duration::from_millis(50)));
    let response = encode_daemon_response(STATUS_DEVICE_BUSY, &[]);
    let _ = write_frame(&mut stream, &response, MAX_DAEMON_RESPONSE_BYTES);
}

/// Runs the daemon connection accept loop with a bounded, UID-aware worker pool.
///
/// # Errors
///
/// Returns [`io::Error`] if accepting connections fails persistently.
pub fn run_daemon_loop(listener: &UnixListener, config: &DaemonConfig) -> io::Result<()> {
    run_daemon_loop_until(listener, config, None)
}

fn run_daemon_loop_until(
    listener: &UnixListener,
    config: &DaemonConfig,
    shutdown: Option<&AtomicBool>,
) -> io::Result<()> {
    listener.set_nonblocking(true)?;
    let (sender, workers) = spawn_workers(config)?;
    let ingress = Arc::new(IngressState::default());
    let result = loop {
        reap_auth_child(config);
        if shutdown.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
            break Ok(());
        }
        match listener.accept() {
            Ok((stream, _)) => {
                let deadline = connection_deadline(config);
                let Ok(peer) = peer_credentials(stream.as_raw_fd()) else {
                    continue;
                };
                let Some(permit) = ingress.try_acquire(peer.uid) else {
                    reject_busy(stream);
                    continue;
                };
                let job = DaemonJob {
                    stream,
                    peer,
                    deadline,
                    permit,
                };
                if let Err(TrySendError::Full(job) | TrySendError::Disconnected(job)) =
                    sender.try_send(job)
                {
                    let DaemonJob { stream, permit, .. } = job;
                    reject_busy(stream);
                    drop(permit);
                }
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(5));
            }
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => break Err(error),
        }
    };
    drop(sender);
    for worker in workers {
        let _ = worker.join();
    }
    if let Ok(mut slot) = config.auth_child.lock()
        && let Some(child) = slot.as_mut()
    {
        let _ = child.kill();
    }
    let reap_deadline = Instant::now() + PAM_RESPONSE_RESERVE;
    while Instant::now() < reap_deadline {
        reap_auth_child(config);
        if config.auth_child.lock().is_ok_and(|slot| slot.is_none()) {
            break;
        }
        thread::sleep(Duration::from_millis(2));
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn freshness_request_has_separate_version_and_is_strictly_caller_bound() {
        let mut frame = vec![0, 3, OP_FRESHNESS, 0];
        frame.extend_from_slice(&1000_u32.to_be_bytes());
        frame.extend_from_slice(&[7; 32]);
        let request = decode_daemon_request(&frame).unwrap();
        assert_eq!(request.target_uid(), 1000);
        for peer_uid in [0, 1001] {
            let peer = PeerCredentials {
                uid: peer_uid,
                gid: peer_uid,
                pid: 1,
            };
            assert_eq!(
                dispatch_request(&request, &peer, &DaemonConfig::default()),
                vec![0, 3, STATUS_ACCESS_DENIED, 0, 0]
            );
        }
        let peer = PeerCredentials {
            uid: 1000,
            gid: 1000,
            pid: 1,
        };
        assert_eq!(
            dispatch_request(&request, &peer, &DaemonConfig::default()),
            vec![0, 3, STATUS_SUCCESS, 0, 0]
        );
        frame[1] = 2;
        assert!(decode_daemon_request(&frame).is_err());
        frame[1] = 3;
        frame[3] = 1;
        assert!(decode_daemon_request(&frame).is_err());
        frame[3] = 0;
        frame.pop();
        assert!(decode_daemon_request(&frame).is_err());
    }

    #[test]
    fn pam_rate_limit_is_per_uid_and_expires_after_the_window() {
        let limiter = AuthRateLimiter::default();
        let start = Instant::now();
        for _ in 0..PAM_ATTEMPTS_PER_WINDOW {
            assert!(limiter.allow_at(1000, start));
        }
        assert!(!limiter.allow_at(1000, start));
        assert!(limiter.allow_at(1001, start));
        assert!(limiter.allow_at(1000, start + PAM_ATTEMPT_WINDOW));
    }

    #[test]
    fn ingress_limits_total_and_per_peer_connections_and_releases_permits() {
        let ingress = Arc::new(IngressState::default());
        let first = ingress.try_acquire(1000).expect("first peer slot");
        let second = ingress.try_acquire(1000).expect("second peer slot");
        assert!(ingress.try_acquire(1000).is_none());
        let third = ingress.try_acquire(1001).expect("third global slot");
        let fourth = ingress.try_acquire(1001).expect("fourth global slot");
        assert!(ingress.try_acquire(1002).is_none());

        drop(first);
        assert!(ingress.try_acquire(1000).is_some());
        drop(second);
        drop(third);
        drop(fourth);
        assert!(ingress.try_acquire(1002).is_some());
    }

    #[test]
    fn a_positive_authentication_clears_the_uid_rate_limit() {
        let limiter = AuthRateLimiter::default();
        let start = Instant::now();
        for _ in 0..PAM_ATTEMPTS_PER_WINDOW {
            assert!(limiter.allow_at(1000, start));
        }
        assert!(!limiter.allow_at(1000, start));
        limiter.clear(1000);
        assert!(limiter.allow_at(1000, start));
    }

    #[test]
    fn peer_authorization_allows_owner_or_root() {
        // Root (UID 0) is authorized for system authentication services (SDDM, PAM)
        assert!(is_authorized(0, 1000));
        assert!(is_authorized(0, 0));
        // Peer matching target UID is authorized
        assert!(is_authorized(1000, 1000));
        // Unprivileged cross-UID calls are strictly denied
        assert!(!is_authorized(1001, 1000));
        assert!(!is_authorized(1000, 1001));
    }

    #[test]
    fn request_decode_and_response_round_trip() {
        let mut req_bytes = Vec::new();
        req_bytes.extend_from_slice(&DAEMON_PROTOCOL_VERSION.to_be_bytes());
        req_bytes.push(OP_PAM_AUTH);
        req_bytes.push(AUTH_TARGET_SDDM);
        req_bytes.extend_from_slice(&1000_u32.to_be_bytes());
        req_bytes.extend_from_slice(&2000_u32.to_be_bytes());
        req_bytes.extend_from_slice(&4_u16.to_be_bytes());
        req_bytes.extend_from_slice(b"test");

        let parsed = decode_daemon_request(&req_bytes).unwrap();
        assert_eq!(parsed.target_uid(), 1000);
        if let DaemonRequest::PamAuth {
            auth_target,
            timeout_ms,
            username,
            ..
        } = parsed
        {
            assert_eq!(timeout_ms, 2000);
            assert_eq!(auth_target, AUTH_TARGET_SDDM);
            assert_eq!(username, "test");
        } else {
            panic!("unexpected variant");
        }

        let resp = encode_daemon_response(STATUS_SUCCESS, &[1, 2, 3]);
        assert_eq!(resp[2], STATUS_SUCCESS);
        assert_eq!(&resp[4..], &[1, 2, 3]);
    }

    #[test]
    fn cross_uid_dispatch_returns_access_denied() {
        let req = DaemonRequest::Status { target_uid: 1000 };
        let peer = PeerCredentials {
            uid: 1001,
            gid: 1001,
            pid: 12345,
        };
        let config = DaemonConfig::default();
        let resp = dispatch_request(&req, &peer, &config);
        assert_eq!(resp[2], STATUS_ACCESS_DENIED);
    }

    #[test]
    fn cross_uid_requests_are_rejected_for_every_remaining_operation() {
        let attacker = PeerCredentials {
            uid: 1001,
            gid: 1001,
            pid: 9999,
        };
        let config = DaemonConfig::default();

        let reqs = vec![
            DaemonRequest::PamAuth {
                target_uid: 1000,
                auth_target: AUTH_TARGET_SDDM,
                timeout_ms: 2000,
                username: "victim".to_string(),
            },
            DaemonRequest::Status { target_uid: 1000 },
        ];

        for req in reqs {
            let resp = dispatch_request(&req, &attacker, &config);
            assert_eq!(
                resp[2], STATUS_ACCESS_DENIED,
                "cross-UID request was not denied"
            );
        }
    }

    #[test]
    fn removed_key_export_and_broad_profile_operations_are_rejected() {
        for opcode in [0x12, 0x13, 0x15] {
            let mut payload = Vec::from(DAEMON_PROTOCOL_VERSION.to_be_bytes());
            payload.push(opcode);
            payload.push(0);
            payload.extend_from_slice(&1000_u32.to_be_bytes());
            assert!(decode_daemon_request(&payload).is_err());
        }
    }

    #[test]
    fn socket_stream_communication_round_trip() {
        let (server_sock, mut client_sock) = UnixStream::pair().unwrap();
        let temp_dir = std::env::temp_dir().join(format!("kfaceauth_test_{}", std::process::id()));
        let _ = fs::create_dir_all(&temp_dir);
        let config = DaemonConfig {
            keys_dir: Some(temp_dir.clone()),
            vault_root: Some(temp_dir.clone()),
            ..DaemonConfig::default()
        };

        let handle = std::thread::spawn(move || {
            handle_client_stream(server_sock, &config).unwrap();
        });

        // Client sends a Status request for own UID
        let current_uid = kfaceauth_crypto_openssl_sys::current_uid();
        let mut req_payload = Vec::new();
        req_payload.extend_from_slice(&DAEMON_PROTOCOL_VERSION.to_be_bytes());
        req_payload.push(OP_STATUS);
        req_payload.push(0);
        req_payload.extend_from_slice(&current_uid.to_be_bytes());

        write_frame(&mut client_sock, &req_payload, MAX_DAEMON_REQUEST_BYTES).unwrap();

        let resp_payload = read_frame(&mut client_sock, MAX_DAEMON_RESPONSE_BYTES).unwrap();
        assert_eq!(resp_payload.len(), 4);
        let resp_version = u16::from_be_bytes([resp_payload[0], resp_payload[1]]);
        assert_eq!(resp_version, DAEMON_PROTOCOL_VERSION);
        assert_eq!(resp_payload[2], STATUS_INTERNAL_ERROR);
        assert!(!temp_dir.join(format!("{current_uid}.key")).exists());

        handle.join().unwrap();
        let _ = fs::remove_dir_all(&temp_dir);
    }

    #[test]
    fn slow_partial_frame_does_not_block_a_concurrent_status_request() {
        let temp_dir = std::env::temp_dir().join(format!(
            "kfaceauth-daemon-deadline-{}-{}",
            std::process::id(),
            kfaceauth_crypto_openssl_sys::current_uid()
        ));
        let _ = fs::remove_dir_all(&temp_dir);
        fs::create_dir_all(&temp_dir).unwrap();
        let config = DaemonConfig {
            keys_dir: Some(temp_dir.join("keys")),
            vault_root: Some(temp_dir.join("vault")),
            max_timeout_ms: 1200,
            ..DaemonConfig::default()
        };
        let peer = PeerCredentials {
            uid: kfaceauth_crypto_openssl_sys::current_uid(),
            gid: 0,
            pid: 12345,
        };

        let (slow_server, mut slow_client) = UnixStream::pair().unwrap();
        let slow_peer = peer;
        let slow_config = config.clone();
        let slow = thread::spawn(move || {
            handle_client_stream_with_peer(
                slow_server,
                slow_peer,
                &slow_config,
                connection_deadline(&slow_config),
            )
        });
        slow_client.write_all(&[0, 0]).unwrap();

        let (status_server, mut status_client) = UnixStream::pair().unwrap();
        let status_peer = peer;
        let status_config = config.clone();
        let status = thread::spawn(move || {
            handle_client_stream_with_peer(
                status_server,
                status_peer,
                &status_config,
                connection_deadline(&status_config),
            )
        });
        let mut request = Vec::new();
        request.extend_from_slice(&DAEMON_PROTOCOL_VERSION.to_be_bytes());
        request.push(OP_STATUS);
        request.push(0);
        request.extend_from_slice(&kfaceauth_crypto_openssl_sys::current_uid().to_be_bytes());
        write_frame(&mut status_client, &request, MAX_DAEMON_REQUEST_BYTES).unwrap();

        let start = Instant::now();
        let response = read_frame(&mut status_client, MAX_DAEMON_RESPONSE_BYTES).unwrap();
        assert_eq!(response[2], STATUS_INTERNAL_ERROR);
        assert!(start.elapsed() < Duration::from_millis(600));
        status.join().unwrap().unwrap();

        assert!(slow.join().unwrap().is_err());
        drop(slow_client);
        fs::remove_dir_all(temp_dir).unwrap();
    }

    #[test]
    fn decoder_rejects_malformed_requests() {
        let valid = [
            DAEMON_PROTOCOL_VERSION.to_be_bytes().as_slice(),
            &[OP_PAM_AUTH, AUTH_TARGET_SDDM],
            &1000_u32.to_be_bytes(),
            &2000_u32.to_be_bytes(),
            &4_u16.to_be_bytes(),
            b"test",
        ]
        .concat();
        assert!(decode_daemon_request(&valid).is_ok());
        for index in [3, 14] {
            let mut invalid = valid.clone();
            invalid[index] = 255;
            assert!(decode_daemon_request(&invalid).is_err());
        }
        for timeout in [0_u32, 2001, u32::MAX] {
            let mut invalid = valid.clone();
            invalid[8..12].copy_from_slice(&timeout.to_be_bytes());
            assert!(decode_daemon_request(&invalid).is_err());
        }
        let mut trailing = valid.clone();
        trailing.push(0);
        assert!(decode_daemon_request(&trailing).is_err());
        assert!(decode_daemon_request(&valid[..valid.len() - 1]).is_err());
        let mut nul = valid.clone();
        nul[14] = 0;
        assert!(decode_daemon_request(&nul).is_err());
        let mut status = valid[..8].to_vec();
        status[2] = OP_STATUS;
        status[3] = 0;
        assert!(decode_daemon_request(&status).is_ok());
        status.push(0);
        assert!(decode_daemon_request(&status).is_err());
        let mut maximum = valid[..14].to_vec();
        maximum[12..14].copy_from_slice(&255_u16.to_be_bytes());
        maximum.resize(MAX_DAEMON_REQUEST_BYTES, b'x');
        for timeout in [1_u32, DEFAULT_PAM_TIMEOUT_MS] {
            maximum[8..12].copy_from_slice(&timeout.to_be_bytes());
            assert!(decode_daemon_request(&maximum).is_ok());
        }
        maximum[12..14].copy_from_slice(&256_u16.to_be_bytes());
        assert!(decode_daemon_request(&maximum).is_err());
        let mut empty = valid[..14].to_vec();
        empty[12..14].copy_from_slice(&0_u16.to_be_bytes());
        assert!(decode_daemon_request(&empty).is_err());
        let mut oversized = valid;
        oversized.resize(MAX_DAEMON_REQUEST_BYTES + 1, b'x');
        assert!(decode_daemon_request(&oversized).is_err());
    }

    struct WorkerFixture {
        root: PathBuf,
        config: DaemonConfig,
    }

    impl WorkerFixture {
        fn new(mode: &str) -> Self {
            use std::os::unix::fs::PermissionsExt;
            let nonce = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let root = std::env::temp_dir()
                .join(format!("kfaceauth-process-{}-{nonce}", std::process::id()));
            fs::create_dir(&root).unwrap();
            let script = root.join("fake-auth-worker");
            fs::write(&script, include_str!("../tests/fake_auth_worker.py")).unwrap();
            fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
            let config = DaemonConfig {
                keys_dir: Some(root.clone()),
                worker_path: Some(script),
                synthetic_policy: true,
                camera_device: Some(mode.to_owned()),
                ..DaemonConfig::default()
            };
            Self { root, config }
        }

        fn request(&self, timeout_ms: u32) -> u8 {
            let request = DaemonRequest::PamAuth {
                target_uid: 1000,
                auth_target: AUTH_TARGET_SDDM,
                timeout_ms,
                username: "test".to_owned(),
            };
            dispatch_request(
                &request,
                &PeerCredentials {
                    uid: 1000,
                    gid: 1000,
                    pid: 1,
                },
                &self.config,
            )[2]
        }

        fn wait_reaped(&self) {
            let deadline = Instant::now() + Duration::from_millis(500);
            loop {
                reap_auth_child(&self.config);
                if self.config.auth_child.lock().unwrap().is_none() {
                    return;
                }
                assert!(Instant::now() < deadline, "killed worker should be reaped");
                thread::sleep(Duration::from_millis(2));
            }
        }
    }

    impl Drop for WorkerFixture {
        fn drop(&mut self) {
            if let Some(child) = self.config.auth_child.lock().unwrap().as_mut() {
                let _ = child.kill();
            }
            self.wait_reaped();
            fs::remove_dir_all(&self.root).unwrap();
        }
    }

    #[test]
    fn replacement_with_restored_mode_rejects_late_positive_worker() {
        if kfaceauth_crypto_openssl_sys::effective_uid() != 0 {
            return;
        }
        for change in ["restore", "same", "other-user", "missing", "unsafe"] {
            let mut fixture = WorkerFixture::new("controlled");
            let directory = fs::File::open(&fixture.root).unwrap();
            kfaceauth_crypto_openssl_sys::set_fd_permissions(&directory, 0, 0o755, "root").unwrap();
            let directory = Arc::new(directory);
            let mut policy = kfaceauth_templates::auth_policy::AuthPolicy::default();
            policy
                .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::Manual)
                .unwrap();
            policy.write_atomic(&directory).unwrap();
            fixture.config.policy_directory = Some(directory.clone());
            let script = fixture.root.join("fake-auth-worker");
            fs::write(&script, "#!/usr/bin/python3\nimport os, pathlib, sys, time\nsys.stdin.buffer.read()\np = pathlib.Path(os.environ['KFACEAUTH_KEYS_DIR'])\n(p / 'started').touch()\nwhile not (p / 'release').exists(): time.sleep(0.002)\nsys.stdout.buffer.write(bytes([0,2,0,0]))\n").unwrap();
            thread::scope(|scope| {
                let attempt = scope.spawn(|| fixture.request(3000));
                let deadline = Instant::now() + Duration::from_secs(2);
                while !fixture.root.join("started").exists() {
                    assert!(Instant::now() < deadline);
                    thread::sleep(Duration::from_millis(2));
                }
                match change {
                    "restore" => {
                        policy
                            .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::Off)
                            .unwrap();
                        policy.write_atomic(&directory).unwrap();
                        policy
                            .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::Manual)
                            .unwrap();
                        policy.write_atomic(&directory).unwrap();
                    }
                    "same" => policy.write_atomic(&directory).unwrap(),
                    "other-user" => {
                        policy
                            .set_mode(1001, AuthTarget::PlasmaLock, AuthPolicyMode::Manual)
                            .unwrap();
                        policy.write_atomic(&directory).unwrap();
                    }
                    "missing" => fs::remove_file(fixture.root.join("policy.json")).unwrap(),
                    "unsafe" => {
                        use std::os::unix::fs::PermissionsExt;
                        fs::set_permissions(
                            fixture.root.join("policy.json"),
                            fs::Permissions::from_mode(0o666),
                        )
                        .unwrap();
                    }
                    _ => unreachable!(),
                }
                fs::write(fixture.root.join("release"), b"").unwrap();
                assert_eq!(attempt.join().unwrap(), STATUS_ACCESS_DENIED);
                assert_eq!(
                    fixture
                        .config
                        .rate_limiter
                        .attempts
                        .lock()
                        .unwrap()
                        .get(&1000)
                        .unwrap()
                        .len(),
                    1
                );
            });
        }
    }

    #[test]
    fn worker_path_alone_does_not_bypass_policy() {
        let mut fixture = WorkerFixture::new("progress");
        fixture.config.synthetic_policy = false;
        if kfaceauth_crypto_openssl_sys::effective_uid() == 0 {
            let directory = fs::File::open(&fixture.root).unwrap();
            kfaceauth_crypto_openssl_sys::set_fd_permissions(&directory, 0, 0o755, "root").unwrap();
            fixture.config.policy_directory = Some(Arc::new(directory));
            assert_eq!(fixture.request(2000), STATUS_ACCESS_DENIED);
            assert!(fixture.config.auth_child.lock().unwrap().is_none());
        }
    }

    #[test]
    fn hung_worker_is_killed_reaped_and_next_attempt_recovers() {
        let fixture = WorkerFixture::new("hang-once");
        let started = Instant::now();
        assert_eq!(fixture.request(300), STATUS_TIMEOUT);
        assert!(started.elapsed() < Duration::from_millis(600));
        fixture.wait_reaped();
        assert_eq!(fixture.request(2000), STATUS_SUCCESS);
        assert!(fixture.config.auth_child.lock().unwrap().is_none());
    }

    #[test]
    fn accept_loop_reaps_timed_out_worker_without_another_auth_request() {
        let fixture = WorkerFixture::new("hang-once");
        assert_eq!(fixture.request(300), STATUS_TIMEOUT);
        assert!(fixture.config.auth_child.lock().unwrap().is_some());
        let listener = UnixListener::bind(fixture.root.join("daemon.sock")).unwrap();
        let config = fixture.config.clone();
        let shutdown = Arc::new(AtomicBool::new(false));
        let worker_shutdown = Arc::clone(&shutdown);
        let server = thread::spawn(move || {
            run_daemon_loop_until(&listener, &config, Some(&worker_shutdown))
        });
        let deadline = Instant::now() + Duration::from_millis(500);
        while fixture.config.auth_child.lock().unwrap().is_some() {
            assert!(
                Instant::now() < deadline,
                "accept loop should reap without another request"
            );
            thread::sleep(Duration::from_millis(2));
        }
        shutdown.store(true, Ordering::Relaxed);
        server.join().unwrap().unwrap();
    }

    #[test]
    fn concurrent_worker_does_not_overlap_camera_work() {
        let fixture = WorkerFixture::new("hang-once");
        let config = fixture.config.clone();
        let first = thread::spawn(move || {
            supervise_auth_worker(
                1000,
                AUTH_TARGET_SDDM,
                2000,
                Instant::now() + Duration::from_millis(350),
                &config,
                None,
            )
        });
        let deadline = Instant::now() + Duration::from_millis(300);
        while !fixture.root.join("started").exists() {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(fixture.request(2000), STATUS_DEVICE_BUSY);
        assert_eq!(first.join().unwrap(), STATUS_TIMEOUT);
        fixture.wait_reaped();
        assert_eq!(fixture.request(2000), STATUS_SUCCESS);
    }

    #[test]
    fn worker_progress_is_forwarded_without_becoming_an_authentication_result() {
        let fixture = WorkerFixture::new("progress");
        let (server_stream, mut client_stream) = UnixStream::pair().unwrap();
        let peer = PeerCredentials {
            uid: 1000,
            gid: 1000,
            pid: 12345,
        };
        let config = fixture.config.clone();
        let server = thread::spawn(move || {
            handle_client_stream_with_peer(
                server_stream,
                peer,
                &config,
                connection_deadline(&config),
            )
        });
        let mut request = Vec::new();
        request.extend_from_slice(&DAEMON_PROTOCOL_VERSION.to_be_bytes());
        request.extend_from_slice(&[OP_PAM_AUTH, AUTH_TARGET_SDDM]);
        request.extend_from_slice(&1000_u32.to_be_bytes());
        request.extend_from_slice(&2000_u32.to_be_bytes());
        request.extend_from_slice(&4_u16.to_be_bytes());
        request.extend_from_slice(b"test");
        write_frame(&mut client_stream, &request, MAX_DAEMON_REQUEST_BYTES).unwrap();

        let progress = read_frame(&mut client_stream, MAX_DAEMON_RESPONSE_BYTES).unwrap();
        assert_eq!(progress[2], STATUS_PROGRESS_LOOKING_FOR_FACE);
        let final_response = read_frame(&mut client_stream, MAX_DAEMON_RESPONSE_BYTES).unwrap();
        assert_eq!(final_response[2], STATUS_SUCCESS);
        server.join().unwrap().unwrap();
    }

    #[test]
    fn client_disconnect_kills_the_active_auth_worker() {
        let fixture = WorkerFixture::new("hang-once");
        let (server_stream, mut client_stream) = UnixStream::pair().unwrap();
        let peer = PeerCredentials {
            uid: 1000,
            gid: 1000,
            pid: 12345,
        };
        let config = fixture.config.clone();
        let server = thread::spawn(move || {
            handle_client_stream_with_peer(
                server_stream,
                peer,
                &config,
                connection_deadline(&config),
            )
        });
        let mut request = Vec::new();
        request.extend_from_slice(&DAEMON_PROTOCOL_VERSION.to_be_bytes());
        request.extend_from_slice(&[OP_PAM_AUTH, AUTH_TARGET_SDDM]);
        request.extend_from_slice(&1000_u32.to_be_bytes());
        request.extend_from_slice(&2000_u32.to_be_bytes());
        request.extend_from_slice(&4_u16.to_be_bytes());
        request.extend_from_slice(b"test");
        write_frame(&mut client_stream, &request, MAX_DAEMON_REQUEST_BYTES).unwrap();

        let deadline = Instant::now() + Duration::from_millis(300);
        while !fixture.root.join("started").exists() {
            assert!(
                Instant::now() < deadline,
                "authentication worker did not start"
            );
            thread::sleep(Duration::from_millis(2));
        }
        let cancelled_at = Instant::now();
        drop(client_stream);
        let _ = server.join().unwrap();
        assert!(cancelled_at.elapsed() < Duration::from_millis(250));
        fixture.wait_reaped();
    }

    #[test]
    fn crash_invalid_response_and_late_success_fail_closed() {
        for mode in [
            "crash",
            "trailing",
            "reserved",
            "version",
            "status",
            "truncated",
            "late-success",
        ] {
            let fixture = WorkerFixture::new(mode);
            let result = fixture.request(300);
            assert_ne!(result, STATUS_SUCCESS, "fixture {mode}");
            fixture.wait_reaped();
        }
    }

    #[test]
    fn private_worker_rejects_invalid_deadline_and_trailing_input_without_profile_access() {
        for timeout in [0_u32, 2001] {
            let input = [1000_u32.to_be_bytes(), timeout.to_be_bytes()].concat();
            assert!(serve_auth_worker(&mut input.as_slice(), &mut Vec::new()).is_err());
        }
        let mut input = [1000_u32.to_be_bytes(), 1000_u32.to_be_bytes()].concat();
        input.push(0);
        assert!(serve_auth_worker(&mut input.as_slice(), &mut Vec::new()).is_err());
        assert!(serve_auth_worker(&mut input[..4].as_ref(), &mut Vec::new()).is_err());
    }
}
