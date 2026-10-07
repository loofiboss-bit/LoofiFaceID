// SPDX-License-Identifier: GPL-3.0-or-later

//! Root-owned, per-user opt-in policy for the experimental PAM targets.

use std::collections::BTreeMap;
use std::fmt;
use std::fmt::Write as FmtWrite;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

use kfaceauth_crypto_openssl_sys::{
    effective_uid, open_child_directory_nofollow, open_child_file_nofollow,
    open_directory_nofollow, set_fd_permissions,
};

pub const AUTH_POLICY_PATH: &str = "/etc/kfaceauth/policy.json";
const AUTH_POLICY_FILE: &str = "policy.json";
const MAX_POLICY_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum AuthTarget {
    Sddm,
    PlasmaLock,
}

impl AuthTarget {
    #[must_use]
    pub const fn key(self) -> &'static str {
        match self {
            Self::Sddm => "sddm",
            Self::PlasmaLock => "plasma-lock",
        }
    }

    #[must_use]
    pub const fn pam_service(self) -> &'static str {
        match self {
            Self::Sddm => "sddm-kfaceauth",
            Self::PlasmaLock => "kde-kfaceauth",
        }
    }

    fn parse(value: &str) -> Result<Self, AuthPolicyError> {
        match value {
            "sddm" => Ok(Self::Sddm),
            "plasma-lock" => Ok(Self::PlasmaLock),
            _ => Err(AuthPolicyError::Invalid),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AuthPolicyMode {
    Off,
    OnActivity,
    Manual,
}

impl AuthPolicyMode {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::OnActivity => "on-activity",
            Self::Manual => "manual",
        }
    }

    fn parse(value: &str) -> Result<Self, AuthPolicyError> {
        match value {
            "off" => Ok(Self::Off),
            "on-activity" => Ok(Self::OnActivity),
            "manual" => Ok(Self::Manual),
            _ => Err(AuthPolicyError::Invalid),
        }
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct AuthPolicy {
    users: BTreeMap<u32, BTreeMap<AuthTarget, AuthPolicyMode>>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AuthPolicyError {
    Invalid,
    UnsafeFilesystem,
    Io,
}

impl fmt::Display for AuthPolicyError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Invalid => "authentication policy is malformed or unsupported",
            Self::UnsafeFilesystem => "authentication policy filesystem metadata is unsafe",
            Self::Io => "authentication policy could not be read or written",
        })
    }
}

impl std::error::Error for AuthPolicyError {}

/// An attempt-scoped policy descriptor. Holding the open file prevents inode
/// reuse after atomic replacement from making a revoked attempt look current.
#[derive(Debug)]
pub struct AuthPolicySnapshot {
    policy: AuthPolicy,
    file: File,
    identity: (u64, u64, i64, i64, i64, i64, u64),
}

impl AuthPolicySnapshot {
    #[must_use]
    pub fn policy(&self) -> &AuthPolicy {
        &self.policy
    }

    #[must_use]
    pub fn same_file_as(&self, current: &Self) -> bool {
        // Both descriptors remain alive during this comparison.
        self.file.metadata().is_ok_and(|metadata| {
            metadata.dev() == self.identity.0 && metadata.ino() == self.identity.1
        }) && self.identity == current.identity
    }

    /// Opens the system policy; missing policy is an error for an auth attempt.
    /// # Errors
    /// Returns an error for missing, unsafe, malformed or unreadable policy.
    pub fn load_system() -> Result<Self, AuthPolicyError> {
        let etc = open_directory_nofollow(Path::new("/etc"))
            .map_err(|_| AuthPolicyError::UnsafeFilesystem)?;
        validate_root_directory(&etc, false)?;
        let config = open_policy_directory(&etc)?.ok_or(AuthPolicyError::Io)?;
        Self::load_from_directory(&config)
    }

    /// Opens and parses one verified descriptor without following symlinks.
    /// # Errors
    /// Returns an error for missing, unsafe, malformed or unreadable policy.
    pub fn load_from_directory(directory: &File) -> Result<Self, AuthPolicyError> {
        validate_root_directory(directory, true)?;
        let entry = fs::symlink_metadata(descriptor_path(directory).join(AUTH_POLICY_FILE))
            .map_err(|_| AuthPolicyError::Io)?;
        if !entry.is_file() || entry.file_type().is_symlink() {
            return Err(AuthPolicyError::UnsafeFilesystem);
        }
        let mut file = open_child_file_nofollow(directory, AUTH_POLICY_FILE)
            .map_err(|_| AuthPolicyError::UnsafeFilesystem)?;
        let metadata = file.metadata().map_err(|_| AuthPolicyError::Io)?;
        if !metadata.is_file()
            || metadata.uid() != 0
            || metadata.gid() != 0
            || metadata.mode() & 0o7777 != 0o644
            || metadata.nlink() != 1
            || metadata.len() > u64::try_from(MAX_POLICY_BYTES).unwrap_or(u64::MAX)
        {
            return Err(AuthPolicyError::UnsafeFilesystem);
        }
        let identity = policy_identity(&metadata);
        let mut bytes = Vec::new();
        (&mut file)
            .take(u64::try_from(MAX_POLICY_BYTES + 1).unwrap_or(u64::MAX))
            .read_to_end(&mut bytes)
            .map_err(|_| AuthPolicyError::Io)?;
        let policy = AuthPolicy::parse_json(&bytes)?;
        if policy_identity(&file.metadata().map_err(|_| AuthPolicyError::Io)?) != identity {
            return Err(AuthPolicyError::UnsafeFilesystem);
        }
        Ok(Self {
            policy,
            file,
            identity,
        })
    }
}

fn policy_identity(metadata: &fs::Metadata) -> (u64, u64, i64, i64, i64, i64, u64) {
    (
        metadata.dev(),
        metadata.ino(),
        metadata.mtime(),
        metadata.mtime_nsec(),
        metadata.ctime(),
        metadata.ctime_nsec(),
        metadata.len(),
    )
}

impl AuthPolicy {
    #[must_use]
    pub fn mode_for(&self, uid: u32, target: AuthTarget) -> AuthPolicyMode {
        self.users
            .get(&uid)
            .and_then(|targets| targets.get(&target))
            .copied()
            .unwrap_or(AuthPolicyMode::Off)
    }

    /// Changes one target without changing another user's or target's policy.
    /// UID zero is intentionally not an eligible biometric login identity.
    ///
    /// # Errors
    ///
    /// Returns [`AuthPolicyError::Invalid`] when UID zero is selected.
    pub fn set_mode(
        &mut self,
        uid: u32,
        target: AuthTarget,
        mode: AuthPolicyMode,
    ) -> Result<(), AuthPolicyError> {
        if uid == 0 {
            return Err(AuthPolicyError::Invalid);
        }
        if mode == AuthPolicyMode::Off {
            if let Some(targets) = self.users.get_mut(&uid) {
                targets.remove(&target);
                if targets.is_empty() {
                    self.users.remove(&uid);
                }
            }
        } else {
            self.users.entry(uid).or_default().insert(target, mode);
        }
        Ok(())
    }

    #[must_use]
    pub fn to_json(&self) -> String {
        let mut output = String::from("{\"version\":1,\"users\":{");
        for (user_index, (uid, targets)) in self.users.iter().enumerate() {
            if user_index != 0 {
                output.push(',');
            }
            let _ = write!(output, "\"{uid}\":{{");
            for (target_index, (target, mode)) in targets.iter().enumerate() {
                if target_index != 0 {
                    output.push(',');
                }
                let _ = write!(output, "\"{}\":\"{}\"", target.key(), mode.as_str());
            }
            output.push('}');
        }
        output.push_str("}}\n");
        output
    }

    /// Parses only the version-1 policy schema. Unknown and duplicate fields
    /// fail closed so a newer or ambiguous policy is never treated as Off.
    ///
    /// # Errors
    ///
    /// Returns [`AuthPolicyError::Invalid`] for malformed, oversized, or
    /// unsupported policy data.
    pub fn parse_json(bytes: &[u8]) -> Result<Self, AuthPolicyError> {
        if bytes.len() > MAX_POLICY_BYTES || !bytes.is_ascii() {
            return Err(AuthPolicyError::Invalid);
        }
        let mut parser = PolicyParser::new(bytes);
        let policy = parser.parse_policy()?;
        parser.skip_whitespace();
        if parser.position != bytes.len() {
            return Err(AuthPolicyError::Invalid);
        }
        Ok(policy)
    }

    /// Loads `/etc/kfaceauth/policy.json` without following any path component
    /// and verifies its root-owned, read-only-to-users metadata. A missing
    /// configuration directory or policy file represents the default all-Off
    /// policy; unsafe or malformed entries remain errors.
    ///
    /// # Errors
    ///
    /// Returns an error when the path or policy file is unsafe, malformed, or
    /// cannot be read.
    pub fn load_system() -> Result<Self, AuthPolicyError> {
        let etc = open_directory_nofollow(Path::new("/etc"))
            .map_err(|_| AuthPolicyError::UnsafeFilesystem)?;
        validate_root_directory(&etc, false)?;
        match open_policy_directory(&etc)? {
            Some(config) => Self::load_from_directory(&config),
            None => Ok(Self::default()),
        }
    }

    /// Loads the fixed policy file from an already-open configuration directory.
    /// This is public so the daemon and administrative helper share one parser
    /// and one metadata contract.
    ///
    /// # Errors
    ///
    /// Returns an error when the directory metadata, policy file metadata, or
    /// serialized policy fails the strict version-1 contract.
    pub fn load_from_directory(directory: &File) -> Result<Self, AuthPolicyError> {
        validate_root_directory(directory, true)?;
        let directory_path = descriptor_path(directory);
        let path = directory_path.join(AUTH_POLICY_FILE);
        match fs::symlink_metadata(&path) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(Self::default()),
            Err(_) => Err(AuthPolicyError::Io),
            Ok(entry) if !entry.is_file() || entry.file_type().is_symlink() => {
                Err(AuthPolicyError::UnsafeFilesystem)
            }
            Ok(_) => {
                AuthPolicySnapshot::load_from_directory(directory).map(|snapshot| snapshot.policy)
            }
        }
    }

    /// Atomically writes a root-owned policy in an already verified config
    /// directory. The caller must serialize updates with the policy lock.
    ///
    /// # Errors
    ///
    /// Returns an error when the caller is not root or staging, permissions,
    /// replacement, or directory synchronization fails.
    pub fn write_atomic(&self, directory: &File) -> Result<(), AuthPolicyError> {
        validate_root_directory(directory, true)?;
        if effective_uid() != 0 {
            return Err(AuthPolicyError::UnsafeFilesystem);
        }
        let directory_path = descriptor_path(directory);
        let final_path = directory_path.join(AUTH_POLICY_FILE);
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|_| AuthPolicyError::Io)?
            .as_nanos();
        let staging_name = format!(".policy.json.{}.{}.tmp", std::process::id(), nonce);
        let staging_path = directory_path.join(&staging_name);
        let serialized = self.to_json();
        let mut staging = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&staging_path)
            .map_err(|_| AuthPolicyError::Io)?;
        let result = (|| {
            staging
                .write_all(serialized.as_bytes())
                .map_err(|_| AuthPolicyError::Io)?;
            staging.sync_all().map_err(|_| AuthPolicyError::Io)?;
            set_fd_permissions(&staging, 0, 0o644, "root").map_err(|_| AuthPolicyError::Io)?;
            drop(staging);
            fs::rename(&staging_path, &final_path).map_err(|_| AuthPolicyError::Io)?;
            directory.sync_all().map_err(|_| AuthPolicyError::Io)
        })();
        if result.is_err() {
            let _ = fs::remove_file(staging_path);
        }
        result
    }
}

fn validate_root_directory(
    directory: &File,
    require_config_mode: bool,
) -> Result<(), AuthPolicyError> {
    let metadata = directory
        .metadata()
        .map_err(|_| AuthPolicyError::UnsafeFilesystem)?;
    let mode = metadata.mode() & 0o777;
    if !metadata.is_dir()
        || metadata.uid() != 0
        || metadata.gid() != 0
        || metadata.mode() & 0o7777 != mode
        || (require_config_mode && mode != 0o755)
    {
        return Err(AuthPolicyError::UnsafeFilesystem);
    }
    Ok(())
}

fn open_policy_directory(etc: &File) -> Result<Option<File>, AuthPolicyError> {
    let path = descriptor_path(etc).join("kfaceauth");
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(_) => Err(AuthPolicyError::Io),
        Ok(entry) if !entry.is_dir() || entry.file_type().is_symlink() => {
            Err(AuthPolicyError::UnsafeFilesystem)
        }
        Ok(_) => open_child_directory_nofollow(etc, "kfaceauth")
            .map(Some)
            .map_err(|_| AuthPolicyError::UnsafeFilesystem),
    }
}

fn descriptor_path(directory: &File) -> PathBuf {
    PathBuf::from(format!("/proc/self/fd/{}", directory.as_raw_fd()))
}

struct PolicyParser<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> PolicyParser<'a> {
    const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0 }
    }

    fn parse_policy(&mut self) -> Result<AuthPolicy, AuthPolicyError> {
        self.expect(b'{')?;
        let mut version_seen = false;
        let mut users = None;
        let mut policy = AuthPolicy::default();
        let mut first = true;
        loop {
            self.skip_whitespace();
            if self.consume(b'}') {
                break;
            }
            if !first {
                self.expect(b',')?;
            }
            first = false;
            let key = self.string()?;
            self.expect(b':')?;
            match key.as_str() {
                "version" if !version_seen => {
                    version_seen = true;
                    if self.number()? != 1 {
                        return Err(AuthPolicyError::Invalid);
                    }
                }
                "users" if users.is_none() => {
                    users = Some(self.parse_users()?);
                }
                _ => return Err(AuthPolicyError::Invalid),
            }
        }
        if !version_seen {
            return Err(AuthPolicyError::Invalid);
        }
        policy.users = users.ok_or(AuthPolicyError::Invalid)?;
        Ok(policy)
    }

    fn parse_users(
        &mut self,
    ) -> Result<BTreeMap<u32, BTreeMap<AuthTarget, AuthPolicyMode>>, AuthPolicyError> {
        self.expect(b'{')?;
        let mut users = BTreeMap::new();
        let mut first = true;
        loop {
            self.skip_whitespace();
            if self.consume(b'}') {
                break;
            }
            if !first {
                self.expect(b',')?;
            }
            first = false;
            let uid_text = self.string()?;
            let uid = uid_text
                .parse::<u32>()
                .map_err(|_| AuthPolicyError::Invalid)?;
            if uid == 0 || uid.to_string() != uid_text {
                return Err(AuthPolicyError::Invalid);
            }
            self.expect(b':')?;
            let targets = self.parse_targets()?;
            if users.insert(uid, targets).is_some() {
                return Err(AuthPolicyError::Invalid);
            }
        }
        Ok(users)
    }

    fn parse_targets(&mut self) -> Result<BTreeMap<AuthTarget, AuthPolicyMode>, AuthPolicyError> {
        self.expect(b'{')?;
        let mut targets = BTreeMap::new();
        let mut first = true;
        loop {
            self.skip_whitespace();
            if self.consume(b'}') {
                break;
            }
            if !first {
                self.expect(b',')?;
            }
            first = false;
            let name = self.string()?;
            let target = AuthTarget::parse(&name)?;
            self.expect(b':')?;
            let mode = AuthPolicyMode::parse(&self.string()?)?;
            if mode == AuthPolicyMode::Off || targets.insert(target, mode).is_some() {
                return Err(AuthPolicyError::Invalid);
            }
        }
        Ok(targets)
    }

    fn string(&mut self) -> Result<String, AuthPolicyError> {
        self.skip_whitespace();
        self.expect(b'"')?;
        let start = self.position;
        while let Some(byte) = self.bytes.get(self.position).copied() {
            if byte == b'"' {
                let value = std::str::from_utf8(&self.bytes[start..self.position])
                    .map_err(|_| AuthPolicyError::Invalid)?;
                self.position += 1;
                return Ok(value.to_owned());
            }
            if byte == b'\\' || byte < 0x20 {
                return Err(AuthPolicyError::Invalid);
            }
            self.position += 1;
        }
        Err(AuthPolicyError::Invalid)
    }

    fn number(&mut self) -> Result<u32, AuthPolicyError> {
        self.skip_whitespace();
        let start = self.position;
        while self
            .bytes
            .get(self.position)
            .is_some_and(u8::is_ascii_digit)
        {
            self.position += 1;
        }
        let bytes = &self.bytes[start..self.position];
        if bytes.is_empty() || (bytes.len() > 1 && bytes[0] == b'0') {
            return Err(AuthPolicyError::Invalid);
        }
        std::str::from_utf8(bytes)
            .ok()
            .and_then(|value| value.parse().ok())
            .ok_or(AuthPolicyError::Invalid)
    }

    fn expect(&mut self, expected: u8) -> Result<(), AuthPolicyError> {
        self.skip_whitespace();
        if self.consume(expected) {
            Ok(())
        } else {
            Err(AuthPolicyError::Invalid)
        }
    }

    fn consume(&mut self, expected: u8) -> bool {
        if self.bytes.get(self.position) == Some(&expected) {
            self.position += 1;
            true
        } else {
            false
        }
    }

    fn skip_whitespace(&mut self) {
        while self
            .bytes
            .get(self.position)
            .is_some_and(u8::is_ascii_whitespace)
        {
            self.position += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temporary_config_directory() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock is after UNIX epoch")
            .as_nanos();
        let root = std::env::temp_dir().join(format!(
            "kfaceauth-auth-policy-{}-{nonce}",
            std::process::id()
        ));
        let config_path = root.join("kfaceauth");
        fs::create_dir_all(&config_path).expect("create policy fixture");
        let config = open_directory_nofollow(&config_path).expect("open policy fixture");
        set_fd_permissions(&config, 0, 0o755, "root").expect("secure policy fixture");
        config_path
    }

    #[test]
    fn snapshot_rejects_atomic_replacement_even_with_identical_contents() {
        if effective_uid() != 0 {
            return;
        }
        let config_path = temporary_config_directory();
        let directory = open_directory_nofollow(&config_path).unwrap();
        let mut policy = AuthPolicy::default();
        policy
            .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::Manual)
            .unwrap();
        policy.write_atomic(&directory).unwrap();
        let snapshot = AuthPolicySnapshot::load_from_directory(&directory).unwrap();
        let unchanged = AuthPolicySnapshot::load_from_directory(&directory).unwrap();
        assert!(snapshot.same_file_as(&unchanged));
        policy.write_atomic(&directory).unwrap();
        let replaced = AuthPolicySnapshot::load_from_directory(&directory).unwrap();
        assert_eq!(snapshot.policy(), replaced.policy());
        assert!(!snapshot.same_file_as(&replaced));
        assert_eq!(snapshot.file.metadata().unwrap().nlink(), 0);
        fs::remove_dir_all(config_path.parent().unwrap()).unwrap();
    }

    #[test]
    fn snapshot_missing_unsafe_and_malformed_entries_never_authorize() {
        if effective_uid() != 0 {
            return;
        }
        let config_path = temporary_config_directory();
        let directory = open_directory_nofollow(&config_path).unwrap();
        assert!(AuthPolicySnapshot::load_from_directory(&directory).is_err());
        AuthPolicy::default().write_atomic(&directory).unwrap();
        let path = config_path.join(AUTH_POLICY_FILE);
        fs::set_permissions(&path, fs::Permissions::from_mode(0o666)).unwrap();
        assert!(AuthPolicySnapshot::load_from_directory(&directory).is_err());
        fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();
        fs::write(&path, b"not a policy").unwrap();
        assert!(AuthPolicySnapshot::load_from_directory(&directory).is_err());
        fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();
        assert!(AuthPolicySnapshot::load_from_directory(&directory).is_err());
        fs::remove_dir(&path).unwrap();
        std::os::unix::fs::symlink("outside-policy", &path).unwrap();
        assert!(AuthPolicySnapshot::load_from_directory(&directory).is_err());
        fs::remove_dir_all(config_path.parent().unwrap()).unwrap();
    }

    #[test]
    fn missing_entries_are_off_and_changes_are_independent() {
        let mut policy = AuthPolicy::default();
        policy
            .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::OnActivity)
            .unwrap();
        policy
            .set_mode(1000, AuthTarget::PlasmaLock, AuthPolicyMode::Manual)
            .unwrap();
        policy
            .set_mode(1001, AuthTarget::Sddm, AuthPolicyMode::Manual)
            .unwrap();
        policy
            .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::Off)
            .unwrap();

        assert_eq!(policy.mode_for(1000, AuthTarget::Sddm), AuthPolicyMode::Off);
        assert_eq!(
            policy.mode_for(1000, AuthTarget::PlasmaLock),
            AuthPolicyMode::Manual
        );
        assert_eq!(
            policy.mode_for(1001, AuthTarget::Sddm),
            AuthPolicyMode::Manual
        );
    }

    #[test]
    fn schema_round_trips_with_canonical_json() {
        let input = br#"{"version":1,"users":{"1000":{"sddm":"on-activity","plasma-lock":"manual"},"1001":{"sddm":"manual"}}}"#;
        let policy = AuthPolicy::parse_json(input).unwrap();
        assert_eq!(
            policy.to_json(),
            "{\"version\":1,\"users\":{\"1000\":{\"sddm\":\"on-activity\",\"plasma-lock\":\"manual\"},\"1001\":{\"sddm\":\"manual\"}}}\n"
        );
        assert_eq!(
            AuthPolicy::parse_json(policy.to_json().as_bytes()).unwrap(),
            policy
        );
    }

    #[test]
    fn malformed_unknown_duplicate_and_unsupported_policy_fails_closed() {
        for input in [
            br"{}".as_slice(),
            br#"{"version":2,"users":{}}"#,
            br#"{"version":1,"users":{},"extra":true}"#,
            br#"{"version":1,"version":1,"users":{}}"#,
            br#"{"version":1,"users":{"1000":{"sddm":"off"}}}"#,
            br#"{"version":1,"users":{"1000":{"sudo":"manual"}}}"#,
            br#"{"version":1,"users":{"0":{"sddm":"manual"}}}"#,
            br#"{"version":1,"users":{"01000":{"sddm":"manual"}}}"#,
            br#"{"version":1,"users":{"1000":{"sddm":"manual","sddm":"manual"}}}"#,
        ] {
            assert!(AuthPolicy::parse_json(input).is_err(), "accepted {input:?}");
        }
    }

    #[test]
    fn target_service_mapping_is_exact() {
        assert_eq!(AuthTarget::Sddm.pam_service(), "sddm-kfaceauth");
        assert_eq!(AuthTarget::PlasmaLock.pam_service(), "kde-kfaceauth");
        assert!(AuthTarget::parse("kde").is_err());
        assert!(AuthTarget::parse("sddm-kfaceauth").is_err());
    }

    #[test]
    fn system_policy_file_is_root_owned_exact_mode_and_atomic_round_trip() {
        if effective_uid() != 0 {
            return;
        }
        let config_path = temporary_config_directory();
        let config = open_directory_nofollow(&config_path).expect("open config directory");
        assert_eq!(
            AuthPolicy::load_from_directory(&config).unwrap(),
            AuthPolicy::default()
        );

        let mut policy = AuthPolicy::default();
        policy
            .set_mode(1000, AuthTarget::Sddm, AuthPolicyMode::OnActivity)
            .unwrap();
        policy
            .set_mode(1001, AuthTarget::PlasmaLock, AuthPolicyMode::Manual)
            .unwrap();
        policy.write_atomic(&config).unwrap();

        let metadata = fs::symlink_metadata(config_path.join(AUTH_POLICY_FILE))
            .expect("inspect root policy file");
        assert_eq!(
            (metadata.uid(), metadata.gid(), metadata.mode() & 0o7777),
            (0, 0, 0o644)
        );
        assert_eq!(metadata.nlink(), 1);
        assert_eq!(AuthPolicy::load_from_directory(&config).unwrap(), policy);

        fs::set_permissions(
            config_path.join(AUTH_POLICY_FILE),
            fs::Permissions::from_mode(0o664),
        )
        .expect("make policy group-writable for negative test");
        assert_eq!(
            AuthPolicy::load_from_directory(&config).unwrap_err(),
            AuthPolicyError::UnsafeFilesystem
        );

        fs::remove_dir_all(config_path.parent().unwrap()).expect("remove fixture");
    }

    #[test]
    fn system_policy_loader_rejects_symlinks_and_noncanonical_schema() {
        if effective_uid() != 0 {
            return;
        }
        let config_path = temporary_config_directory();
        let config = open_directory_nofollow(&config_path).expect("open config directory");
        let policy_path = config_path.join(AUTH_POLICY_FILE);
        let outside_path = config_path.parent().unwrap().join("outside.json");
        fs::write(&outside_path, br#"{"version":1,"users":{}}"#).expect("write outside fixture");
        std::os::unix::fs::symlink(&outside_path, &policy_path).expect("create policy symlink");
        assert_eq!(
            AuthPolicy::load_from_directory(&config).unwrap_err(),
            AuthPolicyError::UnsafeFilesystem
        );

        fs::remove_file(&policy_path).expect("remove policy symlink");
        fs::write(&policy_path, br#"{"version":2,"users":{}}"#)
            .expect("write unsupported policy version");
        assert_eq!(
            AuthPolicy::load_from_directory(&config).unwrap_err(),
            AuthPolicyError::Invalid
        );
        fs::remove_dir_all(config_path.parent().unwrap()).expect("remove fixture");
    }

    #[test]
    fn missing_system_policy_directory_is_all_off_but_unsafe_entries_fail_closed() {
        if effective_uid() != 0 {
            return;
        }
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock is after UNIX epoch")
            .as_nanos();
        let root = std::env::temp_dir().join(format!(
            "kfaceauth-auth-policy-etc-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir_all(&root).expect("create /etc fixture");
        let etc = open_directory_nofollow(&root).expect("open /etc fixture");
        set_fd_permissions(&etc, 0, 0o755, "root").expect("secure /etc fixture");

        assert!(open_policy_directory(&etc).unwrap().is_none());

        let outside = root.parent().unwrap().join(format!(
            "kfaceauth-auth-policy-outside-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir(&outside).expect("create outside directory fixture");
        std::os::unix::fs::symlink(&outside, root.join("kfaceauth"))
            .expect("create unsafe configuration symlink");
        assert_eq!(
            open_policy_directory(&etc).unwrap_err(),
            AuthPolicyError::UnsafeFilesystem
        );

        fs::remove_file(root.join("kfaceauth")).expect("remove unsafe symlink");
        fs::write(root.join("kfaceauth"), b"not a directory").expect("create unsafe file entry");
        assert_eq!(
            open_policy_directory(&etc).unwrap_err(),
            AuthPolicyError::UnsafeFilesystem
        );
        fs::remove_dir_all(&root).expect("remove /etc fixture");
        fs::remove_dir_all(outside).expect("remove outside fixture");
    }
}
