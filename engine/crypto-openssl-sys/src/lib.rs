// SPDX-License-Identifier: GPL-3.0-or-later

//! Narrow safe wrapper around Fedora OpenSSL AES-256-GCM and CSPRNG APIs.

#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::c_int;
use std::fmt;

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
    let mut output = [0_u8; N];
    use std::io::Read;
    file.read_exact(&mut output)
        .map_err(|_| CryptoError::ProviderFailure)?;

    let mut openssl_buf = [0_u8; N];
    // SAFETY: the fixed array is uniquely writable for exactly N bytes.
    status_result(unsafe {
        kfaceauth_crypto_random(openssl_buf.as_mut_ptr(), openssl_buf.len())
    })?;

    for (out, open) in output.iter_mut().zip(openssl_buf.iter()) {
        *out ^= *open;
    }
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
        ciphertext.fill(0);
        tag.fill(0);
        return Err(error);
    }
    if ciphertext_size != ciphertext.len() {
        ciphertext.fill(0);
        tag.fill(0);
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
        plaintext.fill(0);
        return Err(error);
    }
    if plaintext_size != plaintext.len() {
        plaintext.fill(0);
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

#[cfg(test)]
mod tests {
    use super::*;

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
}
