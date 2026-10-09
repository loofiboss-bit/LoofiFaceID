// SPDX-License-Identifier: GPL-3.0-or-later

//! Informational freshness of the root-owned system profile. Never authorizes authentication.

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::Path;

use kfaceauth_crypto_openssl_sys::{
    group_id, open_child_directory_nofollow, open_child_file_nofollow, open_directory_nofollow,
    set_fd_permissions, sha256,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FreshnessError;

const MAGIC: &[u8; 8] = b"KFAFRS01";
pub const METADATA_FILE: &str = "identity.freshness";
const METADATA_BYTES: usize = 78;
const MAX_VAULT_BYTES: usize = 16 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum ProfileFreshness {
    Unknown = 0,
    Current = 1,
    Stale = 2,
}

fn read_protected(directory: &File, name: &str, maximum: usize) -> Result<Vec<u8>, FreshnessError> {
    let mut file = open_child_file_nofollow(directory, name).map_err(|_| FreshnessError)?;
    let before = file.metadata().map_err(|_| FreshnessError)?;
    if !before.is_file()
        || before.uid() != 0
        || before.gid() != group_id("kfaceauth").map_err(|_| FreshnessError)?
        || before.mode() & 0o777 != 0o640
        || before.nlink() != 1
        || before.len() == 0
        || before.len() > u64::try_from(maximum).map_err(|_| FreshnessError)?
    {
        return Err(FreshnessError);
    }
    let mut bytes = Vec::new();
    (&mut file)
        .take(u64::try_from(maximum + 1).map_err(|_| FreshnessError)?)
        .read_to_end(&mut bytes)
        .map_err(|_| FreshnessError)?;
    let after = file.metadata().map_err(|_| FreshnessError)?;
    let current = open_child_file_nofollow(directory, name)
        .map_err(|_| FreshnessError)?
        .metadata()
        .map_err(|_| FreshnessError)?;
    if bytes.len() > maximum
        || before.len() != after.len()
        || before.mtime() != after.mtime()
        || before.mtime_nsec() != after.mtime_nsec()
        || before.ctime() != after.ctime()
        || before.ctime_nsec() != after.ctime_nsec()
        || before.dev() != current.dev()
        || before.ino() != current.ino()
    {
        return Err(FreshnessError);
    }
    Ok(bytes)
}

fn decode(bytes: &[u8], uid: u32) -> Result<([u8; 32], [u8; 32]), FreshnessError> {
    if bytes.len() != METADATA_BYTES
        || &bytes[..8] != MAGIC
        || bytes[8..10] != 1_u16.to_be_bytes()
        || bytes[10..14] != uid.to_be_bytes()
    {
        return Err(FreshnessError);
    }
    Ok((
        bytes[14..46].try_into().map_err(|_| FreshnessError)?,
        bytes[46..78].try_into().map_err(|_| FreshnessError)?,
    ))
}

/// Writes metadata inside a private staging directory before its atomic installation.
///
/// # Errors
/// Rejects unsafe ciphertext or a pre-existing metadata file.
pub fn write_staged_metadata(
    directory: &File,
    uid: u32,
    source: [u8; 32],
) -> Result<(), FreshnessError> {
    let installed = sha256(&read_protected(
        directory,
        "identity.vault",
        MAX_VAULT_BYTES,
    )?)
    .map_err(|_| FreshnessError)?;
    let mut bytes = Vec::with_capacity(METADATA_BYTES);
    bytes.extend_from_slice(MAGIC);
    bytes.extend_from_slice(&1_u16.to_be_bytes());
    bytes.extend_from_slice(&uid.to_be_bytes());
    bytes.extend_from_slice(&source);
    bytes.extend_from_slice(&installed);
    let path = format!("/proc/self/fd/{}/{}", directory.as_raw_fd(), METADATA_FILE);
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|_| FreshnessError)?;
    file.write_all(&bytes).map_err(|_| FreshnessError)?;
    set_fd_permissions(&file, 0, 0o640, "kfaceauth").map_err(|_| FreshnessError)?;
    file.sync_all().map_err(|_| FreshnessError)?;
    directory.sync_all().map_err(|_| FreshnessError)?;
    if read_protected(directory, METADATA_FILE, METADATA_BYTES)? != bytes {
        return Err(FreshnessError);
    }
    Ok(())
}

fn verified_source_at(root: &File, uid: u32) -> Result<[u8; 32], FreshnessError> {
    verified_source_at_checked(root, uid, || {})
}

fn verified_source_at_checked(
    root: &File,
    uid: u32,
    after_metadata: impl FnOnce(),
) -> Result<[u8; 32], FreshnessError> {
    let name = uid.to_string();
    let directory = open_child_directory_nofollow(root, &name).map_err(|_| FreshnessError)?;
    let before = directory.metadata().map_err(|_| FreshnessError)?;
    if before.uid() != 0
        || before.gid() != group_id("kfaceauth").map_err(|_| FreshnessError)?
        || before.mode() & 0o777 != 0o750
    {
        return Err(FreshnessError);
    }
    let metadata = read_protected(&directory, METADATA_FILE, METADATA_BYTES)?;
    let (source, expected) = decode(&metadata, uid)?;
    after_metadata();
    let actual = sha256(&read_protected(
        &directory,
        "identity.vault",
        MAX_VAULT_BYTES,
    )?)
    .map_err(|_| FreshnessError)?;
    if actual != expected || read_protected(&directory, METADATA_FILE, METADATA_BYTES)? != metadata
    {
        return Err(FreshnessError);
    }
    let after = open_child_directory_nofollow(root, &name)
        .map_err(|_| FreshnessError)?
        .metadata()
        .map_err(|_| FreshnessError)?;
    if before.dev() != after.dev()
        || before.ino() != after.ino()
        || before.ctime() != after.ctime()
        || before.ctime_nsec() != after.ctime_nsec()
        || before.mtime() != after.mtime()
        || before.mtime_nsec() != after.mtime_nsec()
    {
        return Err(FreshnessError);
    }
    Ok(source)
}

/// Verify the installed transaction using its already anchored directory handle.
#[must_use]
pub fn verify_installed_metadata_at(root: &File, uid: u32) -> bool {
    verified_source_at(root, uid).is_ok()
}

#[must_use]
pub fn profile_freshness(root: &Path, uid: u32, local: &[u8; 32]) -> ProfileFreshness {
    match open_directory_nofollow(root)
        .map_err(|_| FreshnessError)
        .and_then(|directory| verified_source_at(&directory, uid))
    {
        Ok(source) if &source == local => ProfileFreshness::Current,
        Ok(_) => ProfileFreshness::Stale,
        Err(FreshnessError) => ProfileFreshness::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn replacement_during_verification_is_unknown() {
        use kfaceauth_crypto_openssl_sys::effective_uid;
        if effective_uid() != 0 || group_id("kfaceauth").is_err() {
            return;
        }
        let path =
            std::env::temp_dir().join(format!("kfaceauth-freshness-race-{}", std::process::id()));
        std::fs::create_dir(&path).unwrap();
        let root = open_directory_nofollow(&path).unwrap();
        for name in ["1000", "replacement"] {
            std::fs::create_dir(path.join(name)).unwrap();
            let directory = open_child_directory_nofollow(&root, name).unwrap();
            set_fd_permissions(&directory, 0, 0o750, "kfaceauth").unwrap();
            let file = File::create(path.join(name).join("identity.vault")).unwrap();
            (&file).write_all(b"protected ciphertext fixture").unwrap();
            set_fd_permissions(&file, 0, 0o640, "kfaceauth").unwrap();
            write_staged_metadata(&directory, 1000, [7; 32]).unwrap();
        }
        assert!(
            verified_source_at_checked(&root, 1000, || {
                std::fs::rename(path.join("1000"), path.join("previous")).unwrap();
                std::fs::rename(path.join("replacement"), path.join("1000")).unwrap();
            })
            .is_err()
        );
        assert_eq!(
            profile_freshness(&path, 1000, &[7; 32]),
            ProfileFreshness::Current
        );
        std::fs::remove_dir_all(path).unwrap();
    }

    #[test]
    fn metadata_is_versioned_uid_bound_and_exact_length() {
        let mut bytes = Vec::from(*MAGIC);
        bytes.extend_from_slice(&1_u16.to_be_bytes());
        bytes.extend_from_slice(&1000_u32.to_be_bytes());
        bytes.extend_from_slice(&[1; 32]);
        bytes.extend_from_slice(&[2; 32]);
        assert_eq!(decode(&bytes, 1000), Ok(([1; 32], [2; 32])));
        assert!(decode(&bytes, 1001).is_err());
        bytes[8] = 1;
        assert!(decode(&bytes, 1000).is_err());
        bytes[8] = 0;
        bytes.push(0);
        assert!(decode(&bytes, 1000).is_err());
    }
}
