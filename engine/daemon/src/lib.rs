// SPDX-License-Identifier: GPL-3.0-or-later

//! Standalone System Daemon (`kfaceauthd`) supporting systemd socket activation,
//! caller UID isolation via `SO_PEERCRED`, and least-privilege system vault access.

#![forbid(unsafe_code)]

use std::fs;
use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::Duration;

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
pub const SOCKET_FILE_MODE: u32 = 0o660;

pub const MAX_DAEMON_REQUEST_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_DAEMON_RESPONSE_BYTES: usize = 64 * 1024;
pub const DEFAULT_PAM_TIMEOUT_MS: u32 = 2000;

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

#[derive(Clone, Debug)]
pub struct DaemonConfig {
    pub model_root: PathBuf,
    pub keys_dir: Option<PathBuf>,
    pub vault_root: Option<PathBuf>,
    pub camera_device: Option<String>,
    pub test_frame_path: Option<PathBuf>,
    pub max_timeout_ms: u32,
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
        }
    }
}

/// Binds requests to the kernel-authenticated client identity. This generic
/// socket does not allow privileged callers to act on behalf of another UID.
#[must_use]
pub const fn is_authorized(peer_uid: u32, target_uid: u32) -> bool {
    peer_uid == target_uid
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
    let stride = width * 3;
    let expected_len = (stride * height) as usize;
    buffer.truncate(expected_len);
    let format_u8 = u8::try_from(format).map_err(|_| STATUS_DEVICE_BUSY)?;
    Ok((width, height, stride, format_u8, buffer))
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
    timeout_ms: u32,
    model_root: &Path,
) -> u8 {
    let Ok(format) = PixelFormat::try_from(format_byte) else {
        return STATUS_AUTH_FAILED;
    };
    let Ok(image) = ImageView::new(format, width, height, stride, frame_bytes) else {
        return STATUS_AUTH_FAILED;
    };
    let cancellation = CancellationToken::default();
    let Ok(control) = ProcessingControl::with_timeout(
        &cancellation,
        Duration::from_millis(u64::from(timeout_ms)),
    ) else {
        return STATUS_TIMEOUT;
    };
    let Ok(provider) = IdentityProvider::from_model_root(model_root) else {
        return STATUS_INTERNAL_ERROR;
    };
    let embedding = match provider.extract(image, control) {
        Ok(emb) => emb,
        Err(kfaceauth_vision::identity::IdentityError::SpoofDetected(_)) => {
            return STATUS_SPOOF_DETECTED;
        }
        Err(_) => return STATUS_AUTH_FAILED,
    };
    match vault.open_profile(key) {
        Ok(profile) => match profile.verify(&embedding) {
            VerificationResult::Match => STATUS_SUCCESS,
            VerificationResult::Ambiguous | VerificationResult::NoMatch => STATUS_AUTH_FAILED,
        },
        Err(_) => STATUS_AUTH_FAILED,
    }
}

fn handle_pam_request(target_uid: u32, timeout_ms: u32, config: &DaemonConfig) -> (u8, Vec<u8>) {
    let effective_timeout_ms = timeout_ms.min(config.max_timeout_ms);
    let keys_dir = config.keys_dir.as_deref();

    let Ok(key_bytes) = kfaceauth_crypto_openssl_sys::load_master_key_for_uid(target_uid, keys_dir)
    else {
        return (STATUS_AUTH_FAILED, Vec::new());
    };
    let master_key = MasterKey::from_bytes(key_bytes);

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

    let frame_result = if let Some(path) = &config.test_frame_path {
        load_test_frame(path)
    } else {
        capture_camera_frame(config.camera_device.as_deref(), effective_timeout_ms)
    };

    let (width, height, stride, format, frame_bytes) = match frame_result {
        Ok(frame) => frame,
        Err(status) => return (status, Vec::new()),
    };

    let result = verify_frame_against_vault(
        &vault,
        &master_key,
        width,
        height,
        stride,
        format,
        &frame_bytes,
        effective_timeout_ms,
        &config.model_root,
    );
    (result, Vec::new())
}

/// Dispatches a validated daemon request, strictly enforcing peer authorization.
#[must_use]
pub fn dispatch_request(
    request: &DaemonRequest,
    peer: &PeerCredentials,
    config: &DaemonConfig,
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
        } => handle_pam_request(*target_uid, *timeout_ms, config),
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

/// Handles a single incoming client connection on the Unix domain stream socket.
///
/// # Errors
///
/// Returns [`io::Error`] on network/stream I/O failures.
pub fn handle_client_stream(mut stream: UnixStream, config: &DaemonConfig) -> io::Result<()> {
    // Enforce 2.0s I/O deadline on the connection
    let deadline = Duration::from_millis(u64::from(config.max_timeout_ms));
    let _ = stream.set_read_timeout(Some(deadline));
    let _ = stream.set_write_timeout(Some(deadline));

    let peer = peer_credentials(stream.as_raw_fd()).map_err(|e| {
        io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("peer credentials failed: {e:?}"),
        )
    })?;

    let payload = match read_frame(&mut stream, MAX_DAEMON_REQUEST_BYTES) {
        Ok(p) => p,
        Err(err) => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("frame error: {err}"),
            ));
        }
    };

    let response = match decode_daemon_request(&payload) {
        Ok(request) => dispatch_request(&request, &peer, config),
        Err(_) => encode_daemon_response(STATUS_INTERNAL_ERROR, &[]),
    };

    write_frame(&mut stream, &response, MAX_DAEMON_RESPONSE_BYTES).map_err(|e| {
        io::Error::new(
            io::ErrorKind::BrokenPipe,
            format!("response write error: {e}"),
        )
    })?;

    let _ = stream.flush();
    Ok(())
}

/// Runs the daemon connection accept loop.
///
/// # Errors
///
/// Returns [`io::Error`] if accepting connections fails persistently.
pub fn run_daemon_loop(listener: &UnixListener, config: &DaemonConfig) -> io::Result<()> {
    for stream_res in listener.incoming() {
        match stream_res {
            Ok(stream) => {
                let _ = handle_client_stream(stream, config);
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn peer_authorization_is_bound_to_exact_uid() {
        assert!(!is_authorized(0, 1000));
        assert!(is_authorized(0, 0));
        assert!(is_authorized(1000, 1000));
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
}
