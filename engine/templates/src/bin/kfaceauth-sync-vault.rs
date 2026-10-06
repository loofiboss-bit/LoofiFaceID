// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::env;
use std::fs;
use std::fs::File;
use std::io::{self, Read};
use std::os::fd::AsRawFd;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use kfaceauth_crypto_openssl_sys::{
    KEY_BYTES, effective_uid, load_master_key_for_uid, lock_child_file_nonblocking,
    open_child_directory_nofollow, open_child_file_nofollow, open_directory_nofollow,
    seal_master_key, set_fd_permissions,
};
use kfaceauth_templates::auth_policy::{AuthPolicy, AuthPolicyMode, AuthTarget};
use kfaceauth_templates::{MasterKey, Vault, migrate_legacy_vault_with_separate_key};
use zeroize::Zeroize;

const INPUT_MAGIC: &[u8; 8] = b"KFAUTH01";
const MAX_DATA_HOME_BYTES: usize = 4096;
const SYSTEM_GROUP: &str = "kfaceauth";

fn read_user_session_input(target_uid: u32) -> Result<(File, MasterKey), ()> {
    let mut magic = [0_u8; INPUT_MAGIC.len()];
    if io::stdin().read_exact(&mut magic).is_err() || &magic != INPUT_MAGIC {
        eprintln!("Invalid user-session input header");
        return Err(());
    }
    let mut length_bytes = [0_u8; 2];
    if io::stdin().read_exact(&mut length_bytes).is_err() {
        eprintln!("Missing user-data directory length");
        return Err(());
    }
    let path_len = usize::from(u16::from_be_bytes(length_bytes));
    if path_len == 0 || path_len > MAX_DATA_HOME_BYTES {
        eprintln!("Invalid user-data directory length");
        return Err(());
    }
    let mut path_bytes = vec![0_u8; path_len];
    if io::stdin().read_exact(&mut path_bytes).is_err() {
        eprintln!("Truncated user-data directory");
        return Err(());
    }
    let Ok(path_text) = std::str::from_utf8(&path_bytes) else {
        eprintln!("User-data directory is not valid UTF-8");
        return Err(());
    };
    let data_home = PathBuf::from(path_text);
    if !data_home.is_absolute()
        || data_home.components().any(|component| {
            !matches!(
                component,
                std::path::Component::RootDir | std::path::Component::Normal(_)
            )
        })
    {
        eprintln!("User-data directory must be an absolute, normalized path");
        return Err(());
    }
    let Ok(data_home_fd) = open_directory_nofollow(&data_home) else {
        eprintln!("User-data directory is unavailable or contains a symlink");
        return Err(());
    };
    let Ok(metadata) = data_home_fd.metadata() else {
        eprintln!("Could not inspect the user-data directory");
        return Err(());
    };
    if !metadata.is_dir() || metadata.uid() != target_uid || metadata.mode() & 0o022 != 0 {
        eprintln!("User-data directory ownership or permissions are unsafe");
        return Err(());
    }

    let mut key = [0_u8; KEY_BYTES];
    if let Err(error) = io::stdin().read_exact(&mut key) {
        key.zeroize();
        eprintln!("Could not read the user-session key: {error}");
        return Err(());
    }
    let mut trailing = [0_u8; 1];
    let trailing_bytes = match io::stdin().read(&mut trailing) {
        Ok(bytes) => bytes,
        Err(error) => {
            key.zeroize();
            trailing.zeroize();
            eprintln!("Could not validate the user-session input: {error}");
            return Err(());
        }
    };
    if trailing_bytes != 0 {
        key.zeroize();
        trailing.zeroize();
        eprintln!("Unexpected trailing user-session input");
        return Err(());
    }
    trailing.zeroize();
    let user_key = MasterKey::from_bytes(key);
    key.zeroize();
    Ok((data_home_fd, user_key))
}

fn usage() -> &'static str {
    "Usage: kfaceauth-sync-vault --enable-target sddm|plasma-lock [--mode off|on-activity|manual] | --disable-target sddm|plasma-lock | --delete-profile --uid UID"
}

#[cfg(test)]
fn success_message(enabled: bool) -> &'static str {
    if enabled {
        "result=ok state=enabled"
    } else {
        "result=ok state=disabled"
    }
}

struct Options {
    target_uid: u32,
    operation: Operation,
}

enum Operation {
    Enable {
        target: AuthTarget,
        mode: AuthPolicyMode,
    },
    Disable {
        target: AuthTarget,
    },
    DeleteProfile,
}

fn parse_pkexec_uid(value: Option<&str>, requested_uid: Option<&str>) -> Result<u32, ()> {
    let Some(value) = value else {
        eprintln!("PKEXEC_UID is required");
        return Err(());
    };
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        eprintln!("PKEXEC_UID must be a numeric UID");
        return Err(());
    }
    let pkexec_uid = value.parse::<u32>().map_err(|_| {
        eprintln!("PKEXEC_UID is outside the supported range");
    })?;
    if pkexec_uid == 0 || pkexec_uid.to_string() != value {
        eprintln!("PKEXEC_UID must be a canonical, non-root UID");
        return Err(());
    }
    if let Some(requested) = requested_uid {
        if requested.parse::<u32>() != Ok(pkexec_uid) || requested != pkexec_uid.to_string() {
            eprintln!("Requested UID does not match the authorizing user");
            return Err(());
        }
    }
    Ok(pkexec_uid)
}

fn parse_args<I>(arguments: I, pkexec_uid: Option<&str>) -> Result<Option<Options>, ()>
where
    I: IntoIterator<Item = String>,
{
    let mut enable_target: Option<String> = None;
    let mut disable_target: Option<String> = None;
    let mut delete_profile = false;
    let mut requested_mode: Option<String> = None;
    let mut requested_uid: Option<String> = None;

    let mut args = arguments.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--enable-target" if enable_target.is_none() => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --enable-target");
                    return Err(());
                };
                enable_target = Some(value);
            }
            "--disable-target" if disable_target.is_none() => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --disable-target");
                    return Err(());
                };
                disable_target = Some(value);
            }
            "--delete-profile" if !delete_profile => delete_profile = true,
            "--mode" if requested_mode.is_none() => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --mode");
                    return Err(());
                };
                requested_mode = Some(value);
            }
            "--uid" if requested_uid.is_none() => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --uid");
                    return Err(());
                };
                requested_uid = Some(value);
            }
            "--help" | "-h" => {
                println!("{}", usage());
                return Ok(None);
            }
            _ => {
                eprintln!("Unknown argument. {}", usage());
                return Err(());
            }
        }
    }

    let operations = usize::from(enable_target.is_some())
        + usize::from(disable_target.is_some())
        + usize::from(delete_profile);
    if operations != 1 {
        eprintln!("Choose exactly one target operation. {}", usage());
        return Err(());
    }
    let operation = if let Some(target) = enable_target {
        let target = parse_target(&target)?;
        let mode = match requested_mode.as_deref().unwrap_or("on-activity") {
            "off" => AuthPolicyMode::Off,
            "on-activity" => AuthPolicyMode::OnActivity,
            "manual" => AuthPolicyMode::Manual,
            _ => {
                eprintln!("Enable mode must be off, on-activity, or manual");
                return Err(());
            }
        };
        Operation::Enable { target, mode }
    } else if let Some(target) = disable_target {
        if requested_mode.is_some() {
            eprintln!("--mode is valid only with --enable-target");
            return Err(());
        }
        Operation::Disable {
            target: parse_target(&target)?,
        }
    } else {
        if requested_mode.is_some() {
            eprintln!("--mode is invalid with --delete-profile");
            return Err(());
        }
        if requested_uid.is_none() {
            eprintln!("--uid is required with --delete-profile");
            return Err(());
        }
        Operation::DeleteProfile
    };
    let target_uid = parse_pkexec_uid(pkexec_uid, requested_uid.as_deref())?;

    Ok(Some(Options {
        target_uid,
        operation,
    }))
}

fn parse_target(value: &str) -> Result<AuthTarget, ()> {
    match value {
        "sddm" => Ok(AuthTarget::Sddm),
        "plasma-lock" => Ok(AuthTarget::PlasmaLock),
        _ => {
            eprintln!("Target must be sddm or plasma-lock");
            Err(())
        }
    }
}

fn run_setup(target: AuthTarget, target_uid: u32) -> bool {
    let candidates = [
        "/usr/bin/kfaceauth-pam-setup",
        "/usr/local/bin/kfaceauth-pam-setup",
    ];
    for executable in candidates {
        let mut command = std::process::Command::new(executable);
        command
            .arg("--prepare-target")
            .arg(target.key())
            .arg("--uid")
            .arg(target_uid.to_string());
        if let Ok(status) = command.status() {
            return status.success();
        }
    }
    false
}

struct AnchoredDirectories {
    system_root: File,
    config_dir: File,
    keys_dir: File,
}

impl AnchoredDirectories {
    fn production() -> Result<Self, ()> {
        let var_lib = open_directory_nofollow(Path::new("/var/lib")).map_err(|_| {
            eprintln!("Could not open /var/lib without following symlinks");
        })?;
        let system_root = ensure_child_directory(&var_lib, "kfaceauth", 0o750, "kfaceauth")?;

        let etc = open_directory_nofollow(Path::new("/etc")).map_err(|_| {
            eprintln!("Could not open /etc without following symlinks");
        })?;
        let config_dir = ensure_child_directory(&etc, "kfaceauth", 0o755, "root")?;
        let keys_dir = ensure_child_directory(&config_dir, "keys", 0o750, SYSTEM_GROUP)?;
        Ok(Self {
            system_root,
            config_dir,
            keys_dir,
        })
    }

    fn system_root_path(&self) -> PathBuf {
        descriptor_path(&self.system_root)
    }

    fn keys_dir_path(&self) -> PathBuf {
        descriptor_path(&self.keys_dir)
    }
}

fn load_policy(paths: &AnchoredDirectories) -> Result<AuthPolicy, ()> {
    AuthPolicy::load_from_directory(&paths.config_dir).map_err(|error| {
        eprintln!("{error}");
    })
}

fn write_policy(paths: &AnchoredDirectories, policy: &AuthPolicy) -> Result<(), ()> {
    policy.write_atomic(&paths.config_dir).map_err(|error| {
        eprintln!("{error}");
    })
}

fn acquire_policy_lock(paths: &AnchoredDirectories) -> Result<File, ()> {
    lock_child_file_nonblocking(&paths.config_dir, ".policy.lock").map_err(|_| {
        eprintln!("Another authentication-policy transaction is active or the lock is unsafe");
    })
}

fn revoke_user_targets(policy: &mut AuthPolicy, uid: u32) -> Result<(), ()> {
    policy
        .set_mode(uid, AuthTarget::Sddm, AuthPolicyMode::Off)
        .and_then(|()| policy.set_mode(uid, AuthTarget::PlasmaLock, AuthPolicyMode::Off))
        .map_err(|_| {
            eprintln!("Could not revoke the user's authentication targets");
        })
}

fn descriptor_path(directory: &File) -> PathBuf {
    PathBuf::from(format!("/proc/self/fd/{}", directory.as_raw_fd()))
}

fn ensure_child_directory(parent: &File, name: &str, mode: u32, group: &str) -> Result<File, ()> {
    let path = descriptor_path(parent).join(name);
    match fs::create_dir(&path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
        Err(error) => {
            eprintln!("Could not create a fixed KFaceAuth directory: {error}");
            return Err(());
        }
    }
    let directory = open_child_directory_nofollow(parent, name).map_err(|_| {
        eprintln!("A fixed KFaceAuth directory is missing or is a symlink");
    })?;
    set_fd_permissions(&directory, 0, mode, group).map_err(|_| {
        eprintln!("Could not secure a fixed KFaceAuth directory");
    })?;
    let metadata = directory.metadata().map_err(|_| ())?;
    let gid = kfaceauth_crypto_openssl_sys::group_id(group).map_err(|_| ())?;
    if metadata.uid() != 0 || metadata.gid() != gid || metadata.mode() & 0o777 != mode {
        eprintln!("A fixed KFaceAuth directory failed ownership verification");
        return Err(());
    }
    Ok(directory)
}

fn load_or_generate_system_key(
    target_uid: u32,
    keys_dir: &File,
    system_vault_base: &Path,
) -> Option<(MasterKey, bool)> {
    let keys_dir_path = descriptor_path(keys_dir);
    let key_file = keys_dir_path.join(format!("{target_uid}.key"));
    match fs::symlink_metadata(&key_file) {
        Ok(_) => {
            let Ok(file) = open_child_file_nofollow(keys_dir, &format!("{target_uid}.key")) else {
                eprintln!("The existing system-login key is unsafe");
                return None;
            };
            let Ok(metadata) = file.metadata() else {
                eprintln!("Could not inspect the existing system-login key");
                return None;
            };
            let Ok(group) = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP) else {
                eprintln!("The KFaceAuth system group is unavailable");
                return None;
            };
            if !metadata.is_file()
                || metadata.uid() != 0
                || metadata.gid() != group
                || metadata.mode() & 0o777 != 0o640
                || metadata.nlink() != 1
                || metadata.len() != u64::try_from(KEY_BYTES).expect("key length fits u64")
            {
                eprintln!("The existing system-login key failed ownership verification");
                return None;
            }
            let Ok(key) = load_master_key_for_uid(target_uid, Some(&keys_dir_path)) else {
                eprintln!("The existing system-login key is unreadable; refusing to replace it");
                return None;
            };
            Some((MasterKey::from_bytes(key), false))
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            let system_vault = system_vault_base.join(target_uid.to_string());
            match fs::symlink_metadata(&system_vault) {
                Ok(_) => {
                    eprintln!(
                        "An existing system-login profile has no key; refusing to replace it"
                    );
                    return None;
                }
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                Err(_) => {
                    eprintln!("Could not inspect the existing system-login profile");
                    return None;
                }
            }
            let Ok(key) = MasterKey::generate() else {
                eprintln!("Could not generate a system-login key");
                return None;
            };
            Some((key, true))
        }
        Err(_) => {
            eprintln!("Could not inspect the system-login key");
            None
        }
    }
}

fn locate_legacy_vault(target_uid: u32, data_home: &File) -> Vault {
    Vault::user_session_with_root(
        &descriptor_path(data_home).join(kfaceauth_templates::PRODUCT_DIRECTORY),
        target_uid,
    )
}

fn apply_system_profile_permissions(target_uid: u32, system_root: &File) -> Result<(), ()> {
    let directory_name = target_uid.to_string();
    let vault_dir = open_child_directory_nofollow(system_root, &directory_name).map_err(|_| {
        eprintln!("System vault directory is missing or is a symlink");
    })?;
    let vault_file = open_child_file_nofollow(&vault_dir, "identity.vault").map_err(|_| {
        eprintln!("System vault file is missing or is a symlink");
    })?;
    set_fd_permissions(&vault_dir, 0, 0o750, SYSTEM_GROUP).map_err(|_| {
        eprintln!("Could not secure system vault directory ownership");
    })?;
    set_fd_permissions(&vault_file, 0, 0o640, SYSTEM_GROUP).map_err(|_| {
        eprintln!("Could not secure system vault file ownership");
    })?;

    let expected_gid = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).map_err(|_| ())?;
    let directory_metadata = vault_dir.metadata().map_err(|_| ())?;
    let file_metadata = vault_file.metadata().map_err(|_| ())?;
    if directory_metadata.uid() != 0
        || directory_metadata.gid() != expected_gid
        || directory_metadata.mode() & 0o777 != 0o750
        || file_metadata.uid() != 0
        || file_metadata.gid() != expected_gid
        || file_metadata.mode() & 0o777 != 0o640
        || !file_metadata.is_file()
        || file_metadata.nlink() != 1
    {
        eprintln!("System vault ownership or permissions did not verify");
        return Err(());
    }
    Ok(())
}

struct StagingDirectory {
    directory: File,
    path: PathBuf,
    entry_path: PathBuf,
}

impl Drop for StagingDirectory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.entry_path);
    }
}

fn create_staging_directory(system_root: &File) -> Result<StagingDirectory, ()> {
    let nonce = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| ())?
        .as_nanos();
    let name = format!(".kfaceauth-sync-{}-{nonce}", std::process::id());
    let entry_path = descriptor_path(system_root).join(&name);
    fs::create_dir(&entry_path).map_err(|error| {
        eprintln!("Could not create a private system-vault transaction: {error}");
    })?;
    let directory = open_child_directory_nofollow(system_root, &name).map_err(|_| ())?;
    set_fd_permissions(&directory, 0, 0o700, SYSTEM_GROUP).map_err(|_| ())?;
    let path = descriptor_path(&directory);
    Ok(StagingDirectory {
        directory,
        path,
        entry_path,
    })
}

fn validate_and_harden_existing_profile(system_root: &File, target_uid: u32) -> Result<bool, ()> {
    let directory_name = target_uid.to_string();
    let system_root_path = descriptor_path(system_root);
    let final_path = system_root_path.join(&directory_name);
    let directory_entry = match fs::symlink_metadata(&final_path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(_) => return Err(()),
    };
    if !directory_entry.is_dir() || directory_entry.file_type().is_symlink() {
        eprintln!("Existing system vault path is not a directory");
        return Err(());
    }
    let vault_dir = open_child_directory_nofollow(system_root, &directory_name).map_err(|_| {
        eprintln!("Existing system vault directory is unsafe");
    })?;
    let dir_metadata = vault_dir.metadata().map_err(|_| ())?;
    if (dir_metadata.uid() != target_uid && dir_metadata.uid() != 0)
        || (dir_metadata.mode() & 0o777 != 0o750 && dir_metadata.mode() & 0o777 != 0o700)
    {
        eprintln!("Existing system vault directory failed ownership verification");
        return Err(());
    }
    let vault_file = open_child_file_nofollow(&vault_dir, "identity.vault").map_err(|_| {
        eprintln!("Existing system vault file is missing or is a symlink");
    })?;
    let file_metadata = vault_file.metadata().map_err(|_| ())?;
    if !file_metadata.is_file()
        || (file_metadata.uid() != target_uid && file_metadata.uid() != 0)
        || (file_metadata.mode() & 0o777 != 0o640 && file_metadata.mode() & 0o777 != 0o600)
        || file_metadata.nlink() != 1
        || file_metadata.len() > 16 * 1024
    {
        eprintln!("Existing system vault file failed ownership verification");
        return Err(());
    }
    set_fd_permissions(&vault_dir, 0, 0o750, SYSTEM_GROUP).map_err(|_| ())?;
    set_fd_permissions(&vault_file, 0, 0o640, SYSTEM_GROUP).map_err(|_| ())?;
    Ok(true)
}

struct ProfileSwap {
    final_path: PathBuf,
    backup_path: Option<PathBuf>,
    committed: bool,
}

impl ProfileSwap {
    fn commit(mut self) {
        if let Some(backup) = &self.backup_path {
            if let Err(error) = fs::remove_dir_all(backup) {
                eprintln!("Could not remove the protected previous system profile: {error}");
            }
        }
        self.committed = true;
    }

    fn rollback(&mut self) -> Result<(), ()> {
        if self.committed {
            return Ok(());
        }
        if self.final_path.exists() {
            fs::remove_dir_all(&self.final_path).map_err(|error| {
                eprintln!("Could not remove the unactivated system profile: {error}");
            })?;
        }
        if let Some(backup) = &self.backup_path {
            fs::rename(backup, &self.final_path).map_err(|error| {
                eprintln!("Could not restore the previous system profile: {error}");
            })?;
        }
        self.committed = true;
        Ok(())
    }
}

impl Drop for ProfileSwap {
    fn drop(&mut self) {
        let _ = self.rollback();
    }
}

fn install_staged_profile(
    target_uid: u32,
    system_root: &File,
    staging: &StagingDirectory,
) -> Result<ProfileSwap, ()> {
    let system_root_path = descriptor_path(system_root);
    let final_path = system_root_path.join(target_uid.to_string());
    let had_previous = validate_and_harden_existing_profile(system_root, target_uid)?;
    let backup_path = had_previous.then(|| {
        system_root_path.join(format!(
            ".kfaceauth-previous-{}-{}",
            target_uid,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |duration| duration.as_nanos())
        ))
    });
    if let Some(backup) = &backup_path {
        fs::rename(&final_path, backup).map_err(|error| {
            eprintln!("Could not preserve the existing system profile: {error}");
        })?;
    }
    let staged_path = staging.path.join(target_uid.to_string());
    if let Err(error) = fs::rename(&staged_path, &final_path) {
        if let Some(backup) = &backup_path {
            let _ = fs::rename(backup, &final_path);
        }
        eprintln!("Could not atomically install the verified system profile: {error}");
        return Err(());
    }
    Ok(ProfileSwap {
        final_path,
        backup_path,
        committed: false,
    })
}

fn commit_verified_profile(
    target_uid: u32,
    target: AuthTarget,
    requested_mode: AuthPolicyMode,
    paths: &AnchoredDirectories,
    system_key: &MasterKey,
    staging: &StagingDirectory,
) -> Result<(), ()> {
    if !run_setup(target, target_uid) {
        eprintln!("Target preparation failed; the existing system profile was preserved");
        return Err(());
    }
    let _policy_lock = acquire_policy_lock(paths)?;
    let previous_policy = load_policy(paths)?;
    let mut revoked_policy = previous_policy.clone();
    revoke_user_targets(&mut revoked_policy, target_uid)?;
    if write_policy(paths, &revoked_policy).is_err() {
        eprintln!("Could not revoke the current system profile before replacement");
        if write_policy(paths, &previous_policy).is_err() {
            eprintln!("Prior policy restoration failed; authentication remains revoked");
        }
        return Err(());
    }

    let Ok(mut swap) = install_staged_profile(target_uid, &paths.system_root, staging) else {
        if write_policy(paths, &previous_policy).is_err() {
            eprintln!(
                "Profile replacement failed and prior policy restoration failed; authentication remains revoked"
            );
        }
        return Err(());
    };
    let installed_vault = Vault::system_with_root(&paths.system_root_path(), target_uid);
    if installed_vault.validate_integrity(system_key).is_err() {
        eprintln!("The installed system profile failed integrity verification");
        let rollback_ok = swap.rollback().is_ok();
        if rollback_ok && write_policy(paths, &previous_policy).is_err() {
            eprintln!("Prior policy restoration failed; authentication remains revoked");
        }
        return Err(());
    }

    let mut enabled_policy = previous_policy.clone();
    if enabled_policy
        .set_mode(target_uid, target, requested_mode)
        .is_err()
        || write_policy(paths, &enabled_policy).is_err()
    {
        eprintln!("Target policy activation failed; restoring the previous system profile");
        let rollback_ok = swap.rollback().is_ok();
        if rollback_ok && write_policy(paths, &previous_policy).is_err() {
            eprintln!("Prior policy restoration failed; authentication remains revoked");
        }
        return Err(());
    }

    swap.commit();
    Ok(())
}

struct NewKeyGuard {
    path: Option<PathBuf>,
}

impl Drop for NewKeyGuard {
    fn drop(&mut self) {
        if let Some(path) = &self.path {
            let _ = fs::remove_file(path);
        }
    }
}

fn enable_target(
    target_uid: u32,
    target: AuthTarget,
    requested_mode: AuthPolicyMode,
    paths: &AnchoredDirectories,
) -> ExitCode {
    let Ok((data_home, user_key)) = read_user_session_input(target_uid) else {
        return ExitCode::FAILURE;
    };
    let system_root = paths.system_root_path();
    let keys_dir = paths.keys_dir_path();
    let Some((system_key, new_system_key)) =
        load_or_generate_system_key(target_uid, &paths.keys_dir, &system_root)
    else {
        return ExitCode::FAILURE;
    };
    let key_path = keys_dir.join(format!("{target_uid}.key"));
    let mut new_key_guard = NewKeyGuard {
        path: new_system_key.then_some(key_path),
    };
    if new_system_key
        && seal_master_key(target_uid, system_key.sensitive_bytes(), Some(&keys_dir)).is_err()
    {
        eprintln!("Could not store the system-login key");
        return ExitCode::FAILURE;
    }
    if new_system_key {
        let Ok(key_file) = open_child_file_nofollow(&paths.keys_dir, &format!("{target_uid}.key"))
        else {
            eprintln!("Could not verify the newly stored system-login key");
            return ExitCode::FAILURE;
        };
        let Ok(metadata) = key_file.metadata() else {
            return ExitCode::FAILURE;
        };
        let Ok(group) = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP) else {
            return ExitCode::FAILURE;
        };
        if metadata.uid() != 0
            || metadata.gid() != group
            || metadata.mode() & 0o777 != 0o640
            || metadata.len() != u64::try_from(KEY_BYTES).expect("key length fits u64")
        {
            eprintln!("New system-login key did not pass ownership verification");
            return ExitCode::FAILURE;
        }
    }

    let legacy_vault = locate_legacy_vault(target_uid, &data_home);
    let Ok(staging) = create_staging_directory(&paths.system_root) else {
        return ExitCode::FAILURE;
    };
    let stage_dir_name = target_uid.to_string();
    let stage_dir_path = staging.path.join(&stage_dir_name);
    if fs::create_dir(&stage_dir_path).is_err() {
        eprintln!("Could not create the staged system profile directory");
        return ExitCode::FAILURE;
    }
    let Ok(stage_profile_dir) = open_child_directory_nofollow(&staging.directory, &stage_dir_name)
    else {
        eprintln!("Could not open the staged system profile directory safely");
        return ExitCode::FAILURE;
    };
    if set_fd_permissions(&stage_profile_dir, 0, 0o750, SYSTEM_GROUP).is_err() {
        eprintln!("Could not secure the staged system profile directory");
        return ExitCode::FAILURE;
    }

    let system_vault = Vault::system_with_root(&staging.path, target_uid);
    let Ok(_summary) = migrate_legacy_vault_with_separate_key(
        &legacy_vault,
        &system_vault,
        &user_key,
        &system_key,
    ) else {
        eprintln!("Could not validate and provision the system-login profile");
        return ExitCode::FAILURE;
    };
    if apply_system_profile_permissions(target_uid, &staging.directory).is_err()
        || system_vault.validate_integrity(&system_key).is_err()
    {
        eprintln!("The staged system profile failed ownership or integrity verification");
        return ExitCode::FAILURE;
    }

    if commit_verified_profile(
        target_uid,
        target,
        requested_mode,
        paths,
        &system_key,
        &staging,
    )
    .is_err()
    {
        return ExitCode::FAILURE;
    }
    new_key_guard.path = None;
    println!(
        "result=ok state={} mode={}",
        if requested_mode == AuthPolicyMode::Off {
            "disabled"
        } else {
            "enabled"
        },
        requested_mode.as_str()
    );
    ExitCode::SUCCESS
}

fn disable_target(uid: u32, target: AuthTarget, paths: &AnchoredDirectories) -> ExitCode {
    let Ok(_policy_lock) = acquire_policy_lock(paths) else {
        return ExitCode::FAILURE;
    };
    let Ok(mut policy) = load_policy(paths) else {
        return ExitCode::FAILURE;
    };
    if policy.set_mode(uid, target, AuthPolicyMode::Off).is_err()
        || write_policy(paths, &policy).is_err()
    {
        eprintln!("Could not disable the selected user's authentication target");
        return ExitCode::FAILURE;
    }
    println!("result=ok state=disabled");
    ExitCode::SUCCESS
}

fn validate_system_key_for_deletion(keys_dir: &File, uid: u32) -> Result<bool, ()> {
    let key_name = format!("{uid}.key");
    let key_path = descriptor_path(keys_dir).join(&key_name);
    match fs::symlink_metadata(&key_path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(_) => Err(()),
        Ok(entry) if !entry.is_file() || entry.file_type().is_symlink() => Err(()),
        Ok(_) => {
            let key = open_child_file_nofollow(keys_dir, &key_name).map_err(|_| ())?;
            let metadata = key.metadata().map_err(|_| ())?;
            let group = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).map_err(|_| ())?;
            if !metadata.is_file()
                || metadata.uid() != 0
                || metadata.gid() != group
                || metadata.mode() & 0o777 != 0o640
                || metadata.nlink() != 1
                || metadata.len() != u64::try_from(KEY_BYTES).unwrap_or(u64::MAX)
            {
                return Err(());
            }
            Ok(true)
        }
    }
}

fn clear_revoked_copies(uid: u32, paths: &AnchoredDirectories) -> Result<(), ()> {
    let revoked_prefix = format!(".revoked-{uid}-");
    let expected_group = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).map_err(|_| ())?;
    for entry in fs::read_dir(descriptor_path(&paths.system_root)).map_err(|_| ())? {
        let entry = entry.map_err(|_| ())?;
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        if !name.starts_with(&revoked_prefix) {
            continue;
        }
        let metadata = fs::symlink_metadata(entry.path()).map_err(|_| ())?;
        if !metadata.is_dir()
            || metadata.file_type().is_symlink()
            || metadata.uid() != 0
            || metadata.gid() != expected_group
            || metadata.mode() & 0o777 != 0o750
        {
            return Err(());
        }
        fs::remove_dir_all(entry.path()).map_err(|_| ())?;
    }
    for entry in fs::read_dir(descriptor_path(&paths.keys_dir)).map_err(|_| ())? {
        let entry = entry.map_err(|_| ())?;
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        if !name.starts_with(&revoked_prefix)
            || Path::new(&name)
                .extension()
                .is_none_or(|extension| extension != "key")
        {
            continue;
        }
        let metadata = fs::symlink_metadata(entry.path()).map_err(|_| ())?;
        if !metadata.is_file()
            || metadata.file_type().is_symlink()
            || metadata.uid() != 0
            || metadata.gid() != expected_group
            || metadata.mode() & 0o777 != 0o640
            || metadata.nlink() != 1
            || metadata.len() != u64::try_from(KEY_BYTES).unwrap_or(u64::MAX)
        {
            return Err(());
        }
        fs::remove_file(entry.path()).map_err(|_| ())?;
    }
    Ok(())
}

fn delete_system_profile(uid: u32, paths: &AnchoredDirectories) -> ExitCode {
    let Ok(profile_exists) = validate_and_harden_existing_profile(&paths.system_root, uid) else {
        eprintln!("The existing system profile is unsafe; it was preserved");
        return ExitCode::FAILURE;
    };
    let Ok(key_exists) = validate_system_key_for_deletion(&paths.keys_dir, uid) else {
        eprintln!("The existing system key is unsafe; it was preserved");
        return ExitCode::FAILURE;
    };
    let Ok(_policy_lock) = acquire_policy_lock(paths) else {
        return ExitCode::FAILURE;
    };
    let Ok(previous_policy) = load_policy(paths) else {
        return ExitCode::FAILURE;
    };
    let mut revoked_policy = previous_policy.clone();
    if revoke_user_targets(&mut revoked_policy, uid).is_err()
        || write_policy(paths, &revoked_policy).is_err()
    {
        eprintln!("System authentication could not be revoked; no profile data was removed");
        return ExitCode::FAILURE;
    }

    let nonce = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |duration| duration.as_nanos());
    let system_root_path = descriptor_path(&paths.system_root);
    let keys_dir_path = descriptor_path(&paths.keys_dir);
    let profile_path = system_root_path.join(uid.to_string());
    let key_path = keys_dir_path.join(format!("{uid}.key"));
    let profile_backup = system_root_path.join(format!(".revoked-{uid}-{nonce}"));
    let key_backup = keys_dir_path.join(format!(".revoked-{uid}-{nonce}.key"));

    if profile_exists && fs::rename(&profile_path, &profile_backup).is_err() {
        let _ = write_policy(paths, &previous_policy);
        eprintln!("Could not revoke the system profile; it was preserved");
        return ExitCode::FAILURE;
    }
    if key_exists && fs::rename(&key_path, &key_backup).is_err() {
        let profile_restored =
            !profile_exists || fs::rename(&profile_backup, &profile_path).is_ok();
        if profile_restored && write_policy(paths, &previous_policy).is_err() {
            eprintln!(
                "The system profile was restored but prior policy restoration failed; authentication remains revoked"
            );
        }
        eprintln!("Could not revoke the system key; profile data was preserved");
        return ExitCode::FAILURE;
    }
    let sync_ok = paths.system_root.sync_all().is_ok() && paths.keys_dir.sync_all().is_ok();
    if !sync_ok {
        // At this point paths have been renamed out of service. Keep policy Off
        // and refuse local deletion until this operation can be retried.
        eprintln!(
            "System profile revocation could not be made durable; authentication remains off"
        );
        return ExitCode::FAILURE;
    }

    if clear_revoked_copies(uid, paths).is_err() {
        eprintln!(
            "System profile was revoked but cleanup is incomplete; local profile deletion is not complete"
        );
        return ExitCode::FAILURE;
    }
    if paths.system_root.sync_all().is_err() || paths.keys_dir.sync_all().is_err() {
        eprintln!(
            "System profile deletion could not be made durable; retry before deleting the local profile"
        );
        return ExitCode::FAILURE;
    }
    println!("result=ok state=deleted");
    ExitCode::SUCCESS
}

fn main() -> ExitCode {
    let options = match parse_args(env::args().skip(1), env::var("PKEXEC_UID").ok().as_deref()) {
        Ok(Some(options)) => options,
        Ok(None) => return ExitCode::SUCCESS,
        Err(()) => return ExitCode::FAILURE,
    };
    if effective_uid() != 0 {
        eprintln!("This operation must be run through an authorized pkexec action");
        return ExitCode::FAILURE;
    }
    let Ok(paths) = AnchoredDirectories::production() else {
        return ExitCode::FAILURE;
    };
    let lock_name = format!(".sync-{}.lock", options.target_uid);
    let Ok(_target_lock) = lock_child_file_nonblocking(&paths.keys_dir, &lock_name) else {
        eprintln!("Another system-profile operation is active or the lock is unsafe");
        return ExitCode::FAILURE;
    };
    match options.operation {
        Operation::Enable { target, mode } => {
            enable_target(options.target_uid, target, mode, &paths)
        }
        Operation::Disable { target } => disable_target(options.target_uid, target, &paths),
        Operation::DeleteProfile => delete_system_profile(options.target_uid, &paths),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::MetadataExt;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temporary_root() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock is after UNIX epoch")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "kfaceauth-sync-vault-{}-{nonce}",
            std::process::id()
        ))
    }

    fn deletion_fixture(uid: u32) -> Option<(PathBuf, AnchoredDirectories)> {
        if effective_uid() != 0 {
            return None;
        }
        let group = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).ok()?;
        let root = temporary_root();
        let system_root_path = root.join("system");
        let config_dir_path = root.join("config");
        let keys_dir_path = config_dir_path.join("keys");
        fs::create_dir_all(&system_root_path).ok()?;
        fs::create_dir_all(&keys_dir_path).ok()?;

        let system_root = open_directory_nofollow(&system_root_path).ok()?;
        let config_dir = open_directory_nofollow(&config_dir_path).ok()?;
        let keys_dir = open_directory_nofollow(&keys_dir_path).ok()?;
        set_fd_permissions(&system_root, 0, 0o750, SYSTEM_GROUP).ok()?;
        set_fd_permissions(&config_dir, 0, 0o755, "root").ok()?;
        set_fd_permissions(&keys_dir, 0, 0o750, SYSTEM_GROUP).ok()?;

        let profile_path = system_root_path.join(uid.to_string());
        fs::create_dir(&profile_path).ok()?;
        fs::write(
            profile_path.join("identity.vault"),
            b"encrypted profile fixture",
        )
        .ok()?;
        apply_system_profile_permissions(uid, &system_root).ok()?;

        let key_path = keys_dir_path.join(format!("{uid}.key"));
        fs::write(&key_path, vec![0_u8; KEY_BYTES]).ok()?;
        let key_file = open_child_file_nofollow(&keys_dir, &format!("{uid}.key")).ok()?;
        set_fd_permissions(&key_file, 0, 0o640, SYSTEM_GROUP).ok()?;
        let key_metadata = key_file.metadata().ok()?;
        if key_metadata.uid() != 0
            || key_metadata.gid() != group
            || key_metadata.mode() & 0o777 != 0o640
            || key_metadata.len() != u64::try_from(KEY_BYTES).ok()?
        {
            return None;
        }

        let paths = AnchoredDirectories {
            system_root,
            config_dir,
            keys_dir,
        };
        let mut policy = AuthPolicy::default();
        policy
            .set_mode(uid, AuthTarget::Sddm, AuthPolicyMode::OnActivity)
            .ok()?;
        policy
            .set_mode(uid, AuthTarget::PlasmaLock, AuthPolicyMode::Manual)
            .ok()?;
        policy.write_atomic(&paths.config_dir).ok()?;
        Some((root, paths))
    }

    #[test]
    fn success_messages_are_static_and_operation_specific() {
        assert_eq!(success_message(true), "result=ok state=enabled");
        assert_eq!(success_message(false), "result=ok state=disabled");
    }

    fn arguments(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_owned()).collect()
    }

    #[test]
    fn helper_binds_target_uid_to_pkexec_and_rejects_path_overrides() {
        let options = parse_args(arguments(&["--enable-target", "sddm"]), Some("1000"))
            .expect("valid helper arguments")
            .expect("operation is present");
        assert_eq!(options.target_uid, 1000);
        assert!(matches!(
            options.operation,
            Operation::Enable {
                target: AuthTarget::Sddm,
                mode: AuthPolicyMode::OnActivity
            }
        ));

        let manual = parse_args(
            arguments(&[
                "--enable-target",
                "plasma-lock",
                "--mode",
                "manual",
                "--uid",
                "1000",
            ]),
            Some("1000"),
        )
        .expect("valid manual configuration")
        .expect("operation is present");
        assert!(matches!(
            manual.operation,
            Operation::Enable {
                target: AuthTarget::PlasmaLock,
                mode: AuthPolicyMode::Manual
            }
        ));

        let delete = parse_args(
            arguments(&["--delete-profile", "--uid", "1000"]),
            Some("1000"),
        )
        .expect("valid profile deletion")
        .expect("operation is present");
        assert!(matches!(delete.operation, Operation::DeleteProfile));

        for override_args in [
            &["--uid", "1001", "--enable-target", "sddm"][..],
            &["--legacy-root", "/tmp/legacy", "--enable-target", "sddm"][..],
            &["--system-root", "/tmp/system", "--enable-target", "sddm"][..],
            &["--keys-dir", "/tmp/keys", "--enable-target", "sddm"][..],
        ] {
            assert!(parse_args(arguments(override_args), Some("1000")).is_err());
        }
    }

    #[test]
    fn helper_requires_a_valid_pkexec_uid() {
        assert!(parse_args(arguments(&["--disable-target", "sddm"]), None).is_err());
        assert!(parse_args(arguments(&["--disable-target", "sddm"]), Some("1000x")).is_err());
        assert!(parse_args(arguments(&["--disable-target", "sddm"]), Some("4294967296")).is_err());
        assert!(
            parse_args(
                arguments(&["--disable-target", "sddm", "--uid", "1001"]),
                Some("1000")
            )
            .is_err()
        );
        assert!(
            parse_args(
                arguments(&["--enable-target", "sddm", "--mode", "off"]),
                Some("1000")
            )
            .is_ok()
        );
        assert!(
            parse_args(
                arguments(&["--enable-target", "sddm", "--mode", "invalid"]),
                Some("1000")
            )
            .is_err()
        );
        assert!(
            parse_args(
                arguments(&["--delete-profile", "--uid", "1000"]),
                Some("1001")
            )
            .is_err()
        );
    }

    #[test]
    fn missing_key_does_not_replace_an_existing_system_vault() {
        let root = temporary_root();
        let keys_dir = root.join("keys");
        let system_vault_base = root.join("system");
        fs::create_dir_all(&keys_dir).expect("create keys directory");
        let vault_dir = system_vault_base.join("1000");
        fs::create_dir_all(&vault_dir).expect("create system vault directory");
        fs::write(
            vault_dir.join("identity.vault"),
            b"preserve this existing profile",
        )
        .expect("create existing system vault");

        let keys_directory = open_directory_nofollow(&keys_dir).expect("open keys directory");
        assert!(load_or_generate_system_key(1000, &keys_directory, &system_vault_base).is_none());
        assert!(!keys_dir.join("1000.key").exists());
        assert_eq!(
            fs::read(vault_dir.join("identity.vault")).expect("read existing system vault"),
            b"preserve this existing profile"
        );

        fs::remove_dir_all(root).expect("remove temporary test data");
    }

    #[test]
    fn vault_metadata_is_root_owned_read_only_to_target_and_replay_safe() {
        if effective_uid() != 0 || kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).is_err() {
            return;
        }
        let root = temporary_root();
        let system_root_path = root.join("system");
        fs::create_dir_all(&system_root_path).expect("create system root");
        let system_root = open_directory_nofollow(&system_root_path).expect("open system root");
        set_fd_permissions(&system_root, 0, 0o750, SYSTEM_GROUP).expect("protect system root");
        let target_uid = 1000;
        let vault_dir = system_root_path.join(target_uid.to_string());
        fs::create_dir(&vault_dir).expect("create target vault directory");
        fs::write(
            vault_dir.join("identity.vault"),
            b"old authenticated ciphertext",
        )
        .expect("create existing ciphertext");

        apply_system_profile_permissions(target_uid, &system_root)
            .expect("apply descriptor-based vault permissions");
        let directory = fs::symlink_metadata(&vault_dir).expect("inspect vault directory");
        let file =
            fs::symlink_metadata(vault_dir.join("identity.vault")).expect("inspect vault file");
        let group = kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).unwrap();
        assert_eq!(
            (directory.uid(), directory.gid(), directory.mode() & 0o777),
            (0, group, 0o750)
        );
        assert_eq!(
            (file.uid(), file.gid(), file.mode() & 0o777),
            (0, group, 0o640)
        );
        assert_eq!(
            fs::read(vault_dir.join("identity.vault")).expect("read protected ciphertext"),
            b"old authenticated ciphertext"
        );

        fs::remove_dir_all(root).expect("remove test fixture");
    }

    #[test]
    fn failed_target_activation_rolls_back_to_the_previous_profile() {
        if effective_uid() != 0 || kfaceauth_crypto_openssl_sys::group_id(SYSTEM_GROUP).is_err() {
            return;
        }
        let root = temporary_root();
        let system_root_path = root.join("system");
        fs::create_dir_all(&system_root_path).expect("create system root");
        let system_root = open_directory_nofollow(&system_root_path).expect("open system root");
        set_fd_permissions(&system_root, 0, 0o750, SYSTEM_GROUP).expect("protect system root");
        let previous = system_root_path.join("1000");
        fs::create_dir(&previous).expect("create previous profile directory");
        fs::write(previous.join("identity.vault"), b"previous profile")
            .expect("create previous profile");
        let previous_dir = open_child_directory_nofollow(&system_root, "1000")
            .expect("open previous profile directory");
        let previous_file = open_child_file_nofollow(&previous_dir, "identity.vault")
            .expect("open previous profile file");
        set_fd_permissions(&previous_dir, 0, 0o750, SYSTEM_GROUP)
            .expect("protect previous directory");
        set_fd_permissions(&previous_file, 0, 0o640, SYSTEM_GROUP).expect("protect previous file");

        let stage_entry = system_root_path.join(".stage-test");
        fs::create_dir(&stage_entry).expect("create staging root");
        let stage_directory =
            open_child_directory_nofollow(&system_root, ".stage-test").expect("open staging root");
        set_fd_permissions(&stage_directory, 0, 0o700, SYSTEM_GROUP).expect("protect staging root");
        let staged_profile = stage_entry.join("1000");
        fs::create_dir(&staged_profile).expect("create staged profile directory");
        fs::write(staged_profile.join("identity.vault"), b"new profile")
            .expect("create staged profile");
        let staged_dir = open_child_directory_nofollow(&stage_directory, "1000")
            .expect("open staged profile directory");
        let staged_file = open_child_file_nofollow(&staged_dir, "identity.vault")
            .expect("open staged profile file");
        set_fd_permissions(&staged_dir, 0, 0o750, SYSTEM_GROUP).expect("protect staged directory");
        set_fd_permissions(&staged_file, 0, 0o640, SYSTEM_GROUP).expect("protect staged file");
        let staging_path = descriptor_path(&stage_directory);
        let staging = StagingDirectory {
            directory: stage_directory,
            path: staging_path,
            entry_path: stage_entry,
        };

        let mut swap = install_staged_profile(1000, &system_root, &staging)
            .expect("install verified staged profile");
        assert_eq!(
            fs::read(system_root_path.join("1000/identity.vault")).unwrap(),
            b"new profile"
        );
        swap.rollback().expect("restore previous profile");
        assert_eq!(
            fs::read(system_root_path.join("1000/identity.vault")).unwrap(),
            b"previous profile"
        );
        drop(staging);
        fs::remove_dir_all(root).expect("remove test fixture");
    }

    #[test]
    fn profile_delete_revokes_both_targets_before_removing_system_copies() {
        let Some((root, paths)) = deletion_fixture(1000) else {
            return;
        };
        assert_eq!(delete_system_profile(1000, &paths), ExitCode::SUCCESS);
        assert!(!root.join("system/1000").exists());
        assert!(!root.join("config/keys/1000.key").exists());
        let policy = AuthPolicy::load_from_directory(&paths.config_dir)
            .expect("read policy after completed deletion");
        assert_eq!(policy.mode_for(1000, AuthTarget::Sddm), AuthPolicyMode::Off);
        assert_eq!(
            policy.mode_for(1000, AuthTarget::PlasmaLock),
            AuthPolicyMode::Off
        );
        fs::remove_dir_all(root).expect("remove test fixture");
    }

    #[test]
    fn profile_delete_keeps_local_data_eligible_when_system_revoke_fails() {
        let Some((root, paths)) = deletion_fixture(1000) else {
            return;
        };
        let vault_path = root.join("system/1000/identity.vault");
        fs::remove_file(&vault_path).expect("remove vault fixture");
        let outside_path = root.join("outside.vault");
        fs::write(&outside_path, b"must not be followed").expect("write outside fixture");
        std::os::unix::fs::symlink(&outside_path, &vault_path).expect("symlink vault fixture");

        assert_eq!(delete_system_profile(1000, &paths), ExitCode::FAILURE);
        assert!(
            vault_path
                .symlink_metadata()
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert!(root.join("config/keys/1000.key").exists());
        let policy = AuthPolicy::load_from_directory(&paths.config_dir)
            .expect("read policy after failed revoke");
        assert_eq!(
            policy.mode_for(1000, AuthTarget::Sddm),
            AuthPolicyMode::OnActivity
        );
        assert_eq!(
            policy.mode_for(1000, AuthTarget::PlasmaLock),
            AuthPolicyMode::Manual
        );
        fs::remove_dir_all(root).expect("remove test fixture");
    }
}
