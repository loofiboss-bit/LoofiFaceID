// SPDX-License-Identifier: GPL-3.0-or-later

//! Narrow safe wrapper around Fedora OpenSSL AES-256-GCM and CSPRNG APIs.

#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::{CString, c_int};
use std::fmt;
use std::os::fd::{FromRawFd, RawFd};
use std::os::unix::net::UnixListener;
use std::path::Path;

use zeroize::Zeroize;

const STATUS_OK: c_int = 0;
const STATUS_INVALID_ARGUMENT: c_int = 1;
const STATUS_PROVIDER_FAILURE: c_int = 2;
const STATUS_AUTHENTICATION_FAILURE: c_int = 3;

pub const KEY_BYTES: usize = 32;
pub const NONCE_BYTES: usize = 12;
pub const TAG_BYTES: usize = 16;

unsafe extern "C" {
    fn kfaceauth_crypto_random(output: *mut u8, output_size: usize) -> c_int;
    fn kfaceauth_crypto_aes256gcm_encrypt(
        key: *const u8,
        key_size: usize,
        nonce: *const u8,
        nonce_size: usize,
        associated_data: *const u8,
        associated_data_size: usize,
        plaintext: *const u8,
        plaintext_size: usize,
        ciphertext: *mut u8,
        ciphertext_capacity: usize,
        ciphertext_size: *mut usize,
        tag: *mut u8,
        tag_size: usize,
    ) -> c_int;
    fn kfaceauth_crypto_aes256gcm_decrypt(
        key: *const u8,
        key_size: usize,
        nonce: *const u8,
        nonce_size: usize,
        associated_data: *const u8,
        associated_data_size: usize,
        ciphertext: *const u8,
        ciphertext_size: usize,
        tag: *const u8,
        tag_size: usize,
        plaintext: *mut u8,
        plaintext_capacity: usize,
        plaintext_size: *mut usize,
    ) -> c_int;
    fn kfaceauth_crypto_sha256(
        input: *const u8,
        input_size: usize,
        output: *mut u8,
        output_size: usize,
    ) -> c_int;
    fn kfaceauth_current_uid() -> u32;
    fn kfaceauth_socket_peer_cred(
        socket_fd: c_int,
        uid: *mut u32,
        gid: *mut u32,
        pid: *mut i32,
    ) -> c_int;
    fn kfaceauth_drop_privileges(
        username: *const std::ffi::c_char,
        groupname: *const std::ffi::c_char,
    ) -> c_int;
    fn kfaceauth_load_master_key_for_uid(
        uid: u32,
        key_out: *mut u8,
        key_len: usize,
        custom_keys_dir: *const std::ffi::c_char,
    ) -> c_int;
    fn kfaceauth_seal_master_key(
        uid: u32,
        key_in: *const u8,
        key_len: usize,
        custom_keys_dir: *const std::ffi::c_char,
    ) -> c_int;
    fn kfaceauth_systemd_listen_fds() -> c_int;
    fn kfaceauth_set_socket_permissions(
        path: *const std::ffi::c_char,
        mode: u32,
        groupname: *const std::ffi::c_char,
    ) -> c_int;
    fn kfaceauth_v4l2_capture(
        device_path: *const std::ffi::c_char,
        timeout_ms: u32,
        buffer: *mut u8,
        buffer_size: usize,
        width_out: *mut u32,
        height_out: *mut u32,
        format_out: *mut u32,
    ) -> c_int;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CryptoError {
    InvalidArgument,
    ProviderFailure,
    AuthenticationFailure,
    UnknownStatus,
}

impl fmt::Display for CryptoError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::InvalidArgument => "cryptographic provider rejected an argument",
            Self::ProviderFailure => "cryptographic provider failed",
            Self::AuthenticationFailure => "authenticated decryption failed",
            Self::UnknownStatus => "cryptographic provider returned an unknown status",
        })
    }
}

impl std::error::Error for CryptoError {}

fn status_result(status: c_int) -> Result<(), CryptoError> {
    match status {
        STATUS_OK => Ok(()),
        STATUS_INVALID_ARGUMENT => Err(CryptoError::InvalidArgument),
        STATUS_PROVIDER_FAILURE => Err(CryptoError::ProviderFailure),
        STATUS_AUTHENTICATION_FAILURE => Err(CryptoError::AuthenticationFailure),
        _ => Err(CryptoError::UnknownStatus),
    }
}

/// Fills a fixed-size buffer with cryptographically secure random bytes from OS entropy and OpenSSL.
///
/// # Errors
///
/// Returns a stable provider error if the system CSPRNG or OpenSSL rejects the request.
pub fn random<const N: usize>() -> Result<[u8; N], CryptoError> {
    if N == 0 {
        return Err(CryptoError::InvalidArgument);
    }
    let mut file = std::fs::File::open("/dev/urandom").map_err(|_| CryptoError::ProviderFailure)?;
    use std::io::Read;
    let mut bytes = Vec::with_capacity(N);
    file.by_ref()
        .take(u64::try_from(N).map_err(|_| CryptoError::InvalidArgument)?)
        .read_to_end(&mut bytes)
        .map_err(|_| {
            bytes.zeroize();
            CryptoError::ProviderFailure
        })?;
    if bytes.len() != N {
        bytes.zeroize();
        return Err(CryptoError::ProviderFailure);
    }
    let mut output: [u8; N] = match bytes.try_into() {
        Ok(output) => output,
        Err(mut bytes) => {
            bytes.zeroize();
            return Err(CryptoError::ProviderFailure);
        }
    };

    let mut openssl_buf = vec![0_u8; N];
    // SAFETY: openssl_buf is allocated with exactly N bytes and has valid pointer.
    if let Err(error) = status_result(unsafe {
        kfaceauth_crypto_random(openssl_buf.as_mut_ptr(), openssl_buf.len())
    }) {
        openssl_buf.zeroize();
        output.zeroize();
        return Err(error);
    }

    for i in 0..N {
        output[i] ^= openssl_buf[i];
    }
    openssl_buf.zeroize();
    Ok(output)
}

/// Encrypts plaintext with AES-256-GCM and caller-supplied associated data.
///
/// # Errors
///
/// Returns a stable provider error and clears temporary output on failure.
pub fn encrypt(
    key: &[u8; KEY_BYTES],
    nonce: &[u8; NONCE_BYTES],
    associated_data: &[u8],
    plaintext: &[u8],
) -> Result<(Vec<u8>, [u8; TAG_BYTES]), CryptoError> {
    let mut ciphertext = vec![0_u8; plaintext.len()];
    let mut ciphertext_size = 0_usize;
    let mut tag = [0_u8; TAG_BYTES];
    // SAFETY: all slices remain alive for the call and outputs are uniquely
    // writable within their exact capacities.
    let result = status_result(unsafe {
        kfaceauth_crypto_aes256gcm_encrypt(
            key.as_ptr(),
            key.len(),
            nonce.as_ptr(),
            nonce.len(),
            associated_data.as_ptr(),
            associated_data.len(),
            plaintext.as_ptr(),
            plaintext.len(),
            ciphertext.as_mut_ptr(),
            ciphertext.len(),
            &mut ciphertext_size,
            tag.as_mut_ptr(),
            tag.len(),
        )
    });
    if let Err(error) = result {
        ciphertext.zeroize();
        tag.zeroize();
        return Err(error);
    }
    if ciphertext_size != ciphertext.len() {
        ciphertext.zeroize();
        tag.zeroize();
        return Err(CryptoError::ProviderFailure);
    }
    Ok((ciphertext, tag))
}

/// Authenticates and decrypts AES-256-GCM ciphertext.
///
/// # Errors
///
/// Authentication and provider failures return no plaintext.
pub fn decrypt(
    key: &[u8; KEY_BYTES],
    nonce: &[u8; NONCE_BYTES],
    associated_data: &[u8],
    ciphertext: &[u8],
    tag: &[u8; TAG_BYTES],
) -> Result<Vec<u8>, CryptoError> {
    let mut plaintext = vec![0_u8; ciphertext.len()];
    let mut plaintext_size = 0_usize;
    // SAFETY: all input slices remain alive and the plaintext allocation is
    // uniquely writable within its exact capacity.
    let result = status_result(unsafe {
        kfaceauth_crypto_aes256gcm_decrypt(
            key.as_ptr(),
            key.len(),
            nonce.as_ptr(),
            nonce.len(),
            associated_data.as_ptr(),
            associated_data.len(),
            ciphertext.as_ptr(),
            ciphertext.len(),
            tag.as_ptr(),
            tag.len(),
            plaintext.as_mut_ptr(),
            plaintext.len(),
            &mut plaintext_size,
        )
    });
    if let Err(error) = result {
        plaintext.zeroize();
        return Err(error);
    }
    if plaintext_size != plaintext.len() {
        plaintext.zeroize();
        return Err(CryptoError::ProviderFailure);
    }
    Ok(plaintext)
}

#[must_use]
pub fn current_uid() -> u32 {
    // SAFETY: getuid has no pointers and cannot fail.
    unsafe { kfaceauth_current_uid() }
}

/// Computes a SHA-256 digest using OpenSSL's hardware-accelerated EVP implementation.
///
/// # Errors
///
/// Returns [`CryptoError`] if OpenSSL fails or input size exceeds provider limits.
pub fn sha256(input: &[u8]) -> Result<[u8; 32], CryptoError> {
    let mut output = [0_u8; 32];
    // SAFETY: output is a valid 32-byte array and input pointer is valid for input.len() bytes.
    status_result(unsafe {
        kfaceauth_crypto_sha256(
            input.as_ptr(),
            input.len(),
            output.as_mut_ptr(),
            output.len(),
        )
    })?;
    Ok(output)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PeerCredentials {
    pub uid: u32,
    pub gid: u32,
    pub pid: i32,
}

/// Retrieves peer credentials from a connected Unix domain socket.
///
/// # Errors
///
/// Returns [`CryptoError`] if the descriptor is invalid or `getsockopt(SO_PEERCRED)` fails.
pub fn peer_credentials(raw_fd: RawFd) -> Result<PeerCredentials, CryptoError> {
    let mut uid = 0_u32;
    let mut gid = 0_u32;
    let mut pid = 0_i32;
    // SAFETY: pointers point to valid stack variables.
    let status = unsafe { kfaceauth_socket_peer_cred(raw_fd, &mut uid, &mut gid, &mut pid) };
    status_result(status)?;
    Ok(PeerCredentials { uid, gid, pid })
}

/// Permanently drops root privileges to the specified user and group without retaining capabilities.
///
/// # Errors
///
/// Returns [`CryptoError`] if user/group lookup fails or privilege change cannot be verified.
pub fn drop_privileges(username: &str, groupname: &str) -> Result<(), CryptoError> {
    let c_user = CString::new(username).map_err(|_| CryptoError::InvalidArgument)?;
    let c_group = CString::new(groupname).map_err(|_| CryptoError::InvalidArgument)?;
    // SAFETY: c_user and c_group are valid null-terminated C strings.
    let status = unsafe { kfaceauth_drop_privileges(c_user.as_ptr(), c_group.as_ptr()) };
    status_result(status)
}

/// Loads an existing authoritative 32-byte master key for `uid` without creating one.
///
/// If TPM 2.0 is available and functional, tries to unseal from TPM.
/// Otherwise, uses the root-protected system keyring (`/etc/kfaceauth/keys/<uid>.key` with Mode `0600`).
///
/// # Errors
///
/// Returns [`CryptoError`] on I/O, crypto, or permission failures.
pub fn load_master_key_for_uid(
    uid: u32,
    custom_keys_dir: Option<&Path>,
) -> Result<[u8; KEY_BYTES], CryptoError> {
    let mut key = [0_u8; KEY_BYTES];
    let c_dir = match custom_keys_dir {
        Some(path) => {
            let s = path.to_str().ok_or(CryptoError::InvalidArgument)?;
            Some(CString::new(s).map_err(|_| CryptoError::InvalidArgument)?)
        }
        None => None,
    };
    let dir_ptr = c_dir.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    // SAFETY: key is a valid 32-byte buffer and dir_ptr is null or null-terminated string.
    let status =
        unsafe { kfaceauth_load_master_key_for_uid(uid, key.as_mut_ptr(), KEY_BYTES, dir_ptr) };
    if status != STATUS_OK {
        key.zeroize();
        return Err(match status {
            STATUS_INVALID_ARGUMENT => CryptoError::InvalidArgument,
            STATUS_AUTHENTICATION_FAILURE => CryptoError::AuthenticationFailure,
            _ => CryptoError::ProviderFailure,
        });
    }
    Ok(key)
}

/// Seals an authoritative master key for `uid` into the system key store.
///
/// # Errors
///
/// Returns [`CryptoError`] on failure to seal or persist the key.
pub fn seal_master_key(
    uid: u32,
    key: &[u8; KEY_BYTES],
    custom_keys_dir: Option<&Path>,
) -> Result<(), CryptoError> {
    let c_dir = match custom_keys_dir {
        Some(path) => {
            let s = path.to_str().ok_or(CryptoError::InvalidArgument)?;
            Some(CString::new(s).map_err(|_| CryptoError::InvalidArgument)?)
        }
        None => None,
    };
    let dir_ptr = c_dir.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    // SAFETY: key is a valid 32-byte slice and dir_ptr is null or null-terminated string.
    let status = unsafe { kfaceauth_seal_master_key(uid, key.as_ptr(), KEY_BYTES, dir_ptr) };
    status_result(status)
}

/// Checks for systemd socket activation and returns an inherited [`UnixListener`] if present.
///
/// # Errors
///
/// Returns [`CryptoError`] on failure to inspect or construct listener.
pub fn systemd_socket_listener() -> Result<Option<UnixListener>, CryptoError> {
    // SAFETY: FFI call safely reads and unsets LISTEN_PID and LISTEN_FDS in C.
    let fds = unsafe { kfaceauth_systemd_listen_fds() };
    if fds >= 1 {
        // SD_LISTEN_FDS_START is fd 3
        // SAFETY: fd 3 is inherited by systemd socket activation.
        let listener = unsafe { UnixListener::from_raw_fd(3) };
        Ok(Some(listener))
    } else {
        Ok(None)
    }
}

/// Sets permissions and optional group ownership on a socket or file path.
///
/// # Errors
///
/// Returns [`CryptoError`] if path is invalid or chmod fails.
pub fn set_socket_permissions(
    path: &Path,
    mode: u32,
    groupname: Option<&str>,
) -> Result<(), CryptoError> {
    let s = path.to_str().ok_or(CryptoError::InvalidArgument)?;
    let c_path = CString::new(s).map_err(|_| CryptoError::InvalidArgument)?;
    let c_group = match groupname {
        Some(g) => Some(CString::new(g).map_err(|_| CryptoError::InvalidArgument)?),
        None => None,
    };
    let grp_ptr = c_group.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    // SAFETY: valid C strings passed.
    let status = unsafe { kfaceauth_set_socket_permissions(c_path.as_ptr(), mode, grp_ptr) };
    status_result(status)
}

/// Attempts a single V4L2 camera capture.
///
/// # Errors
///
/// Returns [`CryptoError`] if camera device is unavailable, busy, or capture fails.
pub fn v4l2_capture(
    device_path: Option<&str>,
    timeout_ms: u32,
    buffer: &mut [u8],
) -> Result<(u32, u32, u32), CryptoError> {
    let c_path = match device_path {
        Some(d) => Some(CString::new(d).map_err(|_| CryptoError::InvalidArgument)?),
        None => None,
    };
    let dev_ptr = c_path.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    let mut width: u32 = 0;
    let mut height: u32 = 0;
    let mut format: u32 = 0;
    // SAFETY: pointers point to valid stack memory and buffer slice.
    let status = unsafe {
        kfaceauth_v4l2_capture(
            dev_ptr,
            timeout_ms,
            buffer.as_mut_ptr(),
            buffer.len(),
            &mut width,
            &mut height,
            &mut format,
        )
    };
    if status != STATUS_OK {
        return Err(CryptoError::ProviderFailure);
    }
    Ok((width, height, format))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixStream;

    #[test]
    fn round_trip_and_tamper_rejection() {
        let key = random::<KEY_BYTES>().unwrap();
        let nonce = random::<NONCE_BYTES>().unwrap();
        let aad = b"kfaceauth-test-aad";
        let plaintext = b"sensitive template bytes";
        let (mut ciphertext, tag) = encrypt(&key, &nonce, aad, plaintext).unwrap();
        assert_eq!(
            decrypt(&key, &nonce, aad, &ciphertext, &tag).unwrap(),
            plaintext
        );
        ciphertext[0] ^= 1;
        assert_eq!(
            decrypt(&key, &nonce, aad, &ciphertext, &tag),
            Err(CryptoError::AuthenticationFailure)
        );
    }

    #[test]
    fn systemd_socket_listener_returns_none_when_unset() {
        let listener = systemd_socket_listener().unwrap();
        assert!(listener.is_none());
    }

    #[test]
    fn random_nonces_are_unique() {
        assert_ne!(
            random::<NONCE_BYTES>().unwrap(),
            random::<NONCE_BYTES>().unwrap()
        );
    }

    #[test]
    fn sha256_matches_known_vector() {
        let digest = sha256(b"abc").unwrap();
        let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(
            hex,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn peer_credentials_from_unix_socket() {
        use std::os::fd::AsRawFd;
        let (sock_a, _sock_b) = UnixStream::pair().unwrap();
        let creds = peer_credentials(sock_a.as_raw_fd()).unwrap();
        assert_eq!(creds.uid, current_uid());
    }

    #[test]
    fn master_key_load_requires_explicit_provisioning() {
        let tmp = std::env::temp_dir().join(format!("kfaceauth-key-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        let _ = std::fs::create_dir_all(&tmp);
        let uid = 1000_u32;
        assert!(load_master_key_for_uid(uid, Some(&tmp)).is_err());
        let mut key = random::<KEY_BYTES>().unwrap();
        seal_master_key(uid, &key, Some(&tmp)).unwrap();
        let loaded = load_master_key_for_uid(uid, Some(&tmp)).unwrap();
        assert_eq!(key, loaded);
        key.zeroize();

        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn drop_privileges_as_non_root_succeeds() {
        // When unprivileged, drop_privileges is a no-op success.
        // When running as root (e.g. in container build environments), skip in-process
        // privilege dropping so the test runner does not lose write access to temp dirs.
        if current_uid() != 0 {
            assert!(drop_privileges("nobody", "nobody").is_ok());
        }
    }
}
