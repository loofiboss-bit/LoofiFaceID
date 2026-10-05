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
    "Usage: kfaceauth-sync-vault --enable-target sddm|plasma-lock | --disable-target sddm|plasma-lock"
}

struct Options {
    target_uid: u32,
    target: String,
    enable: bool,
}

fn parse_pkexec_uid(value: Option<&str>) -> Result<u32, ()> {
    let Some(value) = value else {
        eprintln!("PKEXEC_UID is required");
        return Err(());
    };
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        eprintln!("PKEXEC_UID must be a numeric UID");
        return Err(());
    }
    value.parse::<u32>().map_err(|_| {
        eprintln!("PKEXEC_UID is outside the supported range");
    })
}

fn parse_args<I>(arguments: I, pkexec_uid: Option<&str>) -> Result<Option<Options>, ()>
where
    I: IntoIterator<Item = String>,
{
    let mut enable_target: Option<String> = None;
    let mut disable_target: Option<String> = None;

    let mut args = arguments.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--enable-target" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --enable-target");
                    return Err(());
                };
                enable_target = Some(value);
            }
            "--disable-target" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --disable-target");
                    return Err(());
                };
                disable_target = Some(value);
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

    if enable_target.is_some() == disable_target.is_some() {
        eprintln!("Choose exactly one target operation. {}", usage());
        return Err(());
    }
    let enable = enable_target.is_some();
    let target = enable_target.or(disable_target).unwrap_or_default();
    if !matches!(target.as_str(), "sddm" | "plasma-lock") {
        eprintln!("Target must be sddm or plasma-lock");
        return Err(());
    }
    let target_uid = parse_pkexec_uid(pkexec_uid)?;

    Ok(Some(Options {
        target_uid,
        target,
        enable,
    }))
}

fn run_setup(target: &str, target_uid: u32, enable: bool) -> bool {
    let candidates = [
        "/usr/bin/kfaceauth-pam-setup",
        "/usr/local/bin/kfaceauth-pam-setup",
    ];
    for executable in candidates {
        let mut command = std::process::Command::new(executable);
        if enable {
            command
                .arg("--enable-target")
                .arg(target)
                .arg("--uid")
                .arg(target_uid.to_string());
        } else {
            command.arg("--disable-target").arg(target);
        }
        if let Ok(status) = command.status() {
            return status.success();
        }
    }
    false
}

struct AnchoredDirectories {
    system_root: File,
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

fn enable_target(options: Options, paths: &AnchoredDirectories) -> ExitCode {
    let target_uid = options.target_uid;
    let target = options.target;
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
    let Ok(summary) = migrate_legacy_vault_with_separate_key(
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

    let Ok(mut swap) = install_staged_profile(target_uid, &paths.system_root, &staging) else {
        return ExitCode::FAILURE;
    };
    let installed_vault = Vault::system_with_root(&system_root, target_uid);
    if installed_vault.validate_integrity(&system_key).is_err() {
        eprintln!("The installed system profile failed integrity verification");
        let _ = swap.rollback();
        return ExitCode::FAILURE;
    }
    if !run_setup(&target, target_uid, true) {
        eprintln!("Target activation failed; restoring the previous system profile");
        let _ = swap.rollback();
        return ExitCode::FAILURE;
    }

    swap.commit();
    new_key_guard.path = None;
    println!("result=ok target={target} samples={}", summary.sample_count);
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
    if !options.enable {
        if run_setup(&options.target, options.target_uid, false) {
            println!("target={} state=disabled", options.target);
            return ExitCode::SUCCESS;
        }
        eprintln!("Could not disable the selected authentication target");
        return ExitCode::FAILURE;
    }
    enable_target(options, &paths)
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

    fn arguments(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_owned()).collect()
    }

    #[test]
    fn helper_binds_target_uid_to_pkexec_and_rejects_path_overrides() {
        let options = parse_args(arguments(&["--enable-target", "sddm"]), Some("1000"))
            .expect("valid helper arguments")
            .expect("operation is present");
        assert_eq!(options.target_uid, 1000);

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
}
