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
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use kfaceauth_crypto_openssl_sys::{PeerCredentials, peer_credentials};
use kfaceauth_protocol::{read_frame, write_frame};
use kfaceauth_templates::{MasterKey, ProfileSummary, Vault, VaultStatus, VerificationResult};
use kfaceauth_vision::identity::IdentityProvider;
use kfaceauth_vision::{
    CancellationToken, ImageView, MAX_FRAME_BYTES, PixelFormat, ProcessingControl,
};

pub const DAEMON_PROTOCOL_VERSION: u16 = 1;
pub const DEFAULT_SOCKET_PATH: &str = "/run/kfaceauth/kfaceauthd.sock";
pub const DEFAULT_DAEMON_USER: &str = "kfaceauth";
pub const DEFAULT_DAEMON_GROUP: &str = "kfaceauth";
pub const SOCKET_FILE_MODE: u32 = 0o666;

pub const MAX_DAEMON_REQUEST_BYTES: usize = 4 * 1024 * 1024;
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

pub const STATUS_SUCCESS: u8 = 0x00;
pub const STATUS_AUTH_FAILED: u8 = 0x01;
pub const STATUS_ACCESS_DENIED: u8 = 0x02;
pub const STATUS_NO_PROFILE: u8 = 0x03;
pub const STATUS_TIMEOUT: u8 = 0x04;
pub const STATUS_DEVICE_BUSY: u8 = 0x05;
pub const STATUS_INTERNAL_ERROR: u8 = 0x06;
pub const STATUS_SPOOF_DETECTED: u8 = 0x07;
pub const STATUS_RATE_LIMITED: u8 = 0x08;

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
    processing_lock: Arc<Mutex<()>>,
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
            processing_lock: Arc::new(Mutex::new(())),
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
        timeout_ms: u32,
        username: String,
    },
    Status {
        target_uid: u32,
    },
}

impl DaemonRequest {
    #[must_use]
    pub const fn target_uid(&self) -> u32 {
        match self {
            Self::PamAuth { target_uid, .. } | Self::Status { target_uid } => *target_uid,
        }
    }
}

/// Decodes one incoming daemon request frame.
///
/// # Errors
///
/// Returns an error string if payload length, version, or format is invalid.
pub fn decode_daemon_request(payload: &[u8]) -> Result<DaemonRequest, &'static str> {
    if payload.len() < 8 {
        return Err("request payload too short");
    }
    let version = u16::from_be_bytes([payload[0], payload[1]]);
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
            let timeout_ms = u32::from_be_bytes([payload[8], payload[9], payload[10], payload[11]]);
            let user_len = usize::from(u16::from_be_bytes([payload[12], payload[13]]));
            if payload.len() < 14 + user_len {
                return Err("truncated username in request");
            }
            let username = String::from_utf8_lossy(&payload[14..14 + user_len]).into_owned();
            Ok(DaemonRequest::PamAuth {
                target_uid,
                timeout_ms,
                username,
            })
        }
        OP_STATUS => Ok(DaemonRequest::Status { target_uid }),
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

fn load_test_frame(path: &Path) -> Result<(u32, u32, u32, u8, Vec<u8>), u8> {
    let data = fs::read(path).map_err(|_| STATUS_DEVICE_BUSY)?;
    if data.len() < 13 {
        return Err(STATUS_DEVICE_BUSY);
    }
    let width = u32::from_be_bytes([data[0], data[1], data[2], data[3]]);
    let height = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
    let stride = u32::from_be_bytes([data[8], data[9], data[10], data[11]]);
    let format = data[12];
    let frame_bytes = data[13..].to_vec();
    Ok((width, height, stride, format, frame_bytes))
}

fn capture_camera_frame(
    device_path: Option<&str>,
    timeout_ms: u32,
) -> Result<(u32, u32, u32, u8, Vec<u8>), u8> {
    let mut buffer = vec![0_u8; MAX_FRAME_BYTES];
    let (width, height, format) =
        kfaceauth_crypto_openssl_sys::v4l2_capture(device_path, timeout_ms, &mut buffer)
            .map_err(|_| STATUS_DEVICE_BUSY)?;
    let format_u8 = u8::try_from(format).map_err(|_| STATUS_DEVICE_BUSY)?;
    let bpp = match format_u8 {
        3 => 1, // PixelFormat::Gray8
        2 => 4, // PixelFormat::Rgba8
        _ => 3, // PixelFormat::Rgb8
    };
    let stride = width * bpp;
    let expected_len = (stride * height) as usize;
    if buffer.len() < expected_len {
        return Err(STATUS_DEVICE_BUSY);
    }
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
    let embedding = match provider.extract(image, control) {
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
    timeout_ms: u32,
    connection_deadline: Instant,
    config: &DaemonConfig,
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
    let keys_dir = config.keys_dir.as_deref();

    let Ok(key_bytes) = kfaceauth_crypto_openssl_sys::load_master_key_for_uid(target_uid, keys_dir)
    else {
        return (STATUS_AUTH_FAILED, Vec::new());
    };
    let master_key = MasterKey::from_bytes(key_bytes);
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
    let target_uid = request.target_uid();
    // Deny requests that attempt to target a UID other than the socket peer.
    if !is_authorized(peer.uid, target_uid) {
        return encode_daemon_response(STATUS_ACCESS_DENIED, &[]);
    }

    let (status, extra) = match request {
        DaemonRequest::PamAuth {
            target_uid,
            timeout_ms,
            ..
        } => {
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
            let worker_config = config.clone();
            let target_uid = *target_uid;
            match run_bounded_job(processing_deadline, move || {
                // OpenCV and V4L2 calls may not be cancellable. Keep this lock
                // in the worker until native processing actually returns, even
                // if the ingress worker has already sent a timeout response.
                let Ok(_processing_guard) = worker_config.processing_lock.try_lock() else {
                    return (STATUS_DEVICE_BUSY, Vec::new());
                };
                if !worker_config.rate_limiter.allow(target_uid) {
                    return (STATUS_RATE_LIMITED, Vec::new());
                }
                let result =
                    handle_pam_request(target_uid, timeout_ms, requested_deadline, &worker_config);
                if result.0 == STATUS_SUCCESS {
                    worker_config.rate_limiter.clear(target_uid);
                }
                result
            }) {
                Ok(result) => result,
                Err(BoundedJobFailure::Timeout) => (STATUS_TIMEOUT, Vec::new()),
                Err(BoundedJobFailure::Spawn | BoundedJobFailure::Disconnected) => {
                    (STATUS_INTERNAL_ERROR, Vec::new())
                }
            }
        }
        DaemonRequest::Status { target_uid } => {
            let keys_dir = config.keys_dir.as_deref();
            match kfaceauth_crypto_openssl_sys::load_master_key_for_uid(*target_uid, keys_dir) {
                Ok(k) => {
                    let master_key = MasterKey::from_bytes(k);
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

#[derive(Debug, Eq, PartialEq)]
enum BoundedJobFailure {
    Timeout,
    Spawn,
    Disconnected,
}

fn run_bounded_job<T, F>(deadline: Instant, work: F) -> Result<T, BoundedJobFailure>
where
    T: Send + 'static,
    F: FnOnce() -> T + Send + 'static,
{
    let remaining = remaining_duration(deadline).map_err(|_| BoundedJobFailure::Timeout)?;
    let (sender, receiver) = mpsc::sync_channel(1);
    thread::Builder::new()
        .name("kfaceauth-pam-work".to_owned())
        .spawn(move || {
            let _ = sender.send(work());
        })
        .map_err(|_| BoundedJobFailure::Spawn)?;
    let result = receiver
        .recv_timeout(remaining)
        .map_err(|error| match error {
            mpsc::RecvTimeoutError::Timeout => BoundedJobFailure::Timeout,
            mpsc::RecvTimeoutError::Disconnected => BoundedJobFailure::Disconnected,
        })?;
    if Instant::now() >= deadline {
        return Err(BoundedJobFailure::Timeout);
    }
    Ok(result)
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

    let response = match decode_daemon_request(&payload) {
        Ok(request) => dispatch_request_until(&request, &peer, config, deadline),
        Err(_) => encode_daemon_response(STATUS_INTERNAL_ERROR, &[]),
    };

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
    result
}

#[cfg(test)]
mod tests {
    use super::*;

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
        req_bytes.push(0);
        req_bytes.extend_from_slice(&1000_u32.to_be_bytes());
        req_bytes.extend_from_slice(&2000_u32.to_be_bytes());
        req_bytes.extend_from_slice(&4_u16.to_be_bytes());
        req_bytes.extend_from_slice(b"test");

        let parsed = decode_daemon_request(&req_bytes).unwrap();
        assert_eq!(parsed.target_uid(), 1000);
        if let DaemonRequest::PamAuth {
            timeout_ms,
            username,
            ..
        } = parsed
        {
            assert_eq!(timeout_ms, 2000);
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
    fn pam_deadline_returns_while_native_work_keeps_processing_lock() {
        let processing_lock = Arc::new(Mutex::new(()));
        let worker_lock = Arc::clone(&processing_lock);
        let (started_tx, started_rx) = std::sync::mpsc::channel();
        let (release_tx, release_rx) = std::sync::mpsc::channel();
        let (finished_tx, finished_rx) = std::sync::mpsc::channel();
        let deadline = Instant::now() + Duration::from_millis(150);

        let worker = thread::spawn(move || {
            run_bounded_job(deadline, move || {
                let guard = worker_lock.lock().unwrap();
                started_tx.send(()).unwrap();
                release_rx.recv().unwrap();
                drop(guard);
                finished_tx.send(()).unwrap();
                STATUS_SUCCESS
            })
        });

        started_rx
            .recv_timeout(Duration::from_millis(100))
            .expect("bounded worker should start before its deadline");
        assert_eq!(worker.join().unwrap(), Err(BoundedJobFailure::Timeout));
        assert!(processing_lock.try_lock().is_err());

        release_tx.send(()).unwrap();
        finished_rx
            .recv_timeout(Duration::from_millis(500))
            .expect("timed-out native work should eventually release the lock");
        assert!(processing_lock.try_lock().is_ok());
    }
}
