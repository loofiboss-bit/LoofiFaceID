// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::env;
use std::fs;
use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use kfaceauth_crypto_openssl_sys::{
    effective_uid, group_id, lock_child_file_nonblocking, open_child_directory_nofollow,
    open_child_file_nofollow, open_directory_nofollow,
};
use kfaceauth_templates::auth_policy::{AuthPolicy, AuthPolicyMode, AuthTarget};

#[derive(Clone, Copy)]
struct Options {
    write: bool,
    uid: u32,
    target: AuthTarget,
    mode: Option<AuthPolicyMode>,
}

fn usage() -> &'static str {
    "Usage: kfaceauth-policy --get --uid UID --target sddm|plasma-lock | --set --uid UID --target sddm|plasma-lock --mode off|on-activity|manual"
}

fn parse_uid(value: &str) -> Result<u32, ()> {
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        eprintln!("UID must be a numeric, non-root UID");
        return Err(());
    }
    let uid = value.parse::<u32>().map_err(|_| {
        eprintln!("UID is outside the supported range");
    })?;
    if uid == 0 || uid.to_string() != value {
        eprintln!("UID must be a canonical, non-root UID");
        return Err(());
    }
    Ok(uid)
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

fn parse_mode(value: &str) -> Result<AuthPolicyMode, ()> {
    match value {
        "off" => Ok(AuthPolicyMode::Off),
        "on-activity" => Ok(AuthPolicyMode::OnActivity),
        "manual" => Ok(AuthPolicyMode::Manual),
        _ => {
            eprintln!("Mode must be off, on-activity, or manual");
            Err(())
        }
    }
}

fn parse_args<I>(arguments: I) -> Result<Option<Options>, ()>
where
    I: IntoIterator<Item = String>,
{
    let mut get = false;
    let mut set = false;
    let mut uid = None;
    let mut target = None;
    let mut mode = None;
    let mut args = arguments.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--get" if !get => get = true,
            "--set" if !set => set = true,
            "--uid" if uid.is_none() => {
                uid = Some(args.next().ok_or_else(|| {
                    eprintln!("Missing value for --uid");
                })?);
            }
            "--target" if target.is_none() => {
                target = Some(args.next().ok_or_else(|| {
                    eprintln!("Missing value for --target");
                })?);
            }
            "--mode" if mode.is_none() => {
                mode = Some(args.next().ok_or_else(|| {
                    eprintln!("Missing value for --mode");
                })?);
            }
            "--help" | "-h" => {
                println!("{}", usage());
                return Ok(None);
            }
            _ => {
                eprintln!("Invalid or duplicate argument. {}", usage());
                return Err(());
            }
        }
    }
    if get == set {
        eprintln!("Choose exactly one of --get or --set. {}", usage());
        return Err(());
    }
    let uid = parse_uid(&uid.ok_or_else(|| eprintln!("--uid is required"))?)?;
    let target = parse_target(&target.ok_or_else(|| eprintln!("--target is required"))?)?;
    let mode = match (set, mode) {
        (true, Some(value)) => Some(parse_mode(&value)?),
        (true, None) => {
            eprintln!("--mode is required with --set");
            return Err(());
        }
        (false, None) => None,
        (false, Some(_)) => {
            eprintln!("--mode is valid only with --set");
            return Err(());
        }
    };
    Ok(Some(Options {
        write: set,
        uid,
        target,
        mode,
    }))
}

fn config_directory() -> Result<Option<File>, ()> {
    let etc = open_directory_nofollow(Path::new("/etc")).map_err(|_| {
        eprintln!("Could not open /etc safely");
    })?;
    let metadata = etc.metadata().map_err(|_| ())?;
    if metadata.uid() != 0 || metadata.mode() & 0o022 != 0 {
        eprintln!("/etc has unsafe ownership or permissions");
        return Err(());
    }
    let descriptor = PathBuf::from(format!("/proc/self/fd/{}", etc.as_raw_fd()));
    let config_path = descriptor.join("kfaceauth");
    match fs::symlink_metadata(&config_path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(_) => Err(()),
        Ok(entry) if !entry.is_dir() || entry.file_type().is_symlink() => {
            eprintln!("KFaceAuth configuration directory is unsafe");
            Err(())
        }
        Ok(_) => {
            let directory = open_child_directory_nofollow(&etc, "kfaceauth").map_err(|_| ())?;
            let metadata = directory.metadata().map_err(|_| ())?;
            if metadata.uid() != 0 || metadata.gid() != 0 || metadata.mode() & 0o777 != 0o755 {
                eprintln!("KFaceAuth configuration directory has unsafe ownership or permissions");
                return Err(());
            }
            Ok(Some(directory))
        }
    }
}

fn profile_is_ready(uid: u32) -> bool {
    let Ok(vault_root) = open_directory_nofollow(Path::new("/var/lib/kfaceauth")) else {
        return false;
    };
    let Ok(profile_dir) = open_child_directory_nofollow(&vault_root, &uid.to_string()) else {
        return false;
    };
    let Ok(vault) = open_child_file_nofollow(&profile_dir, "identity.vault") else {
        return false;
    };
    let Ok(vault_metadata) = vault.metadata() else {
        return false;
    };

    let Ok(etc) = open_directory_nofollow(Path::new("/etc")) else {
        return false;
    };
    let Ok(config) = open_child_directory_nofollow(&etc, "kfaceauth") else {
        return false;
    };
    let Ok(keys) = open_child_directory_nofollow(&config, "keys") else {
        return false;
    };
    let Ok(key) = open_child_file_nofollow(&keys, &format!("{uid}.key")) else {
        return false;
    };
    let Ok(key_metadata) = key.metadata() else {
        return false;
    };
    let Ok(group) = group_id("kfaceauth") else {
        return false;
    };
    [vault_metadata, key_metadata].iter().all(|metadata| {
        metadata.is_file()
            && metadata.uid() == 0
            && metadata.gid() == group
            && metadata.mode() & 0o777 == 0o640
            && metadata.nlink() == 1
            && metadata.len() > 0
    })
}

fn profile_operation_lock(uid: u32) -> Result<File, ()> {
    let etc = open_directory_nofollow(Path::new("/etc")).map_err(|_| ())?;
    let config = open_child_directory_nofollow(&etc, "kfaceauth").map_err(|_| ())?;
    let keys = open_child_directory_nofollow(&config, "keys").map_err(|_| {
        eprintln!("The system-profile key directory is unavailable");
    })?;
    lock_child_file_nonblocking(&keys, &format!(".sync-{uid}.lock")).map_err(|_| {
        eprintln!("Another system-profile operation is active or the lock is unsafe");
    })
}

fn prepare_target(uid: u32, target: AuthTarget) -> bool {
    let candidates = [
        "/usr/bin/kfaceauth-pam-setup",
        "/usr/local/bin/kfaceauth-pam-setup",
    ];
    for executable in candidates {
        if let Ok(status) = std::process::Command::new(executable)
            .arg("--prepare-target")
            .arg(target.key())
            .arg("--uid")
            .arg(uid.to_string())
            .status()
        {
            return status.success();
        }
    }
    false
}

fn authorize_set(uid: u32) -> bool {
    if effective_uid() != 0 {
        eprintln!("Policy updates require administrator authorization");
        return false;
    }
    if let Ok(pkexec_uid) = env::var("PKEXEC_UID") {
        if pkexec_uid.parse::<u32>() != Ok(uid) {
            eprintln!("Policy updates are restricted to the authorizing user's UID");
            return false;
        }
    }
    true
}

fn run(options: Options) -> ExitCode {
    let Options {
        write,
        uid,
        target,
        mode: selected_mode,
    } = options;
    let Ok(directory) = config_directory() else {
        return ExitCode::FAILURE;
    };
    let mut policy = match directory.as_ref() {
        Some(directory) => match AuthPolicy::load_from_directory(directory) {
            Ok(policy) => policy,
            Err(error) => {
                eprintln!("{error}");
                return ExitCode::FAILURE;
            }
        },
        None => AuthPolicy::default(),
    };
    if !write {
        println!("mode={}", policy.mode_for(uid, target).as_str());
        return ExitCode::SUCCESS;
    }
    let Some(mode) = selected_mode else {
        return ExitCode::FAILURE;
    };
    if !authorize_set(uid) {
        return ExitCode::FAILURE;
    }
    if mode == AuthPolicyMode::Off && policy.mode_for(uid, target) == AuthPolicyMode::Off {
        println!("result=ok mode=off");
        return ExitCode::SUCCESS;
    }
    let Some(directory) = directory.as_ref() else {
        eprintln!("KFaceAuth policy cannot be enabled before its system directory is prepared");
        return ExitCode::FAILURE;
    };
    let Ok(_profile_lock) = profile_operation_lock(uid) else {
        return ExitCode::FAILURE;
    };
    if mode != AuthPolicyMode::Off && !profile_is_ready(uid) {
        eprintln!("The root-owned system profile is not provisioned");
        return ExitCode::FAILURE;
    }
    if mode != AuthPolicyMode::Off && !prepare_target(uid, target) {
        eprintln!("The selected dedicated authentication service could not be prepared");
        return ExitCode::FAILURE;
    }
    let Ok(_policy_lock) = lock_child_file_nonblocking(directory, ".policy.lock") else {
        eprintln!("Another policy update is active or the lock is unsafe");
        return ExitCode::FAILURE;
    };
    // Reload under the exclusive lock so concurrent per-user changes are merged.
    policy = match AuthPolicy::load_from_directory(directory) {
        Ok(policy) => policy,
        Err(error) => {
            eprintln!("{error}");
            return ExitCode::FAILURE;
        }
    };
    let mut updated_policy = policy.clone();
    if updated_policy.set_mode(uid, target, mode).is_err() {
        eprintln!("Could not update the selected policy entry");
        return ExitCode::FAILURE;
    }
    if let Err(error) = updated_policy.write_atomic(directory) {
        eprintln!("{error}");
        if mode != AuthPolicyMode::Off {
            if let Err(restore_error) = policy.write_atomic(directory) {
                eprintln!(
                    "Could not restore the previous policy after an activation failure: {restore_error}"
                );
            }
        }
        return ExitCode::FAILURE;
    }
    println!("result=ok mode={}", mode.as_str());
    ExitCode::SUCCESS
}

fn main() -> ExitCode {
    match parse_args(env::args().skip(1)) {
        Ok(Some(options)) => run(options),
        Ok(None) => ExitCode::SUCCESS,
        Err(()) => ExitCode::FAILURE,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_owned()).collect()
    }

    #[test]
    fn query_has_no_mode_mutation_argument_and_set_requires_mode() {
        let get = parse_args(args(&["--get", "--uid", "1000", "--target", "sddm"]))
            .unwrap()
            .unwrap();
        assert!(!get.write);
        assert_eq!(get.uid, 1000);
        assert_eq!(get.target, AuthTarget::Sddm);
        assert!(parse_args(args(&["--set", "--uid", "1000", "--target", "sddm"])).is_err());
        assert!(
            parse_args(args(&[
                "--get", "--uid", "1000", "--target", "sddm", "--mode", "manual"
            ]))
            .is_err()
        );
    }

    #[test]
    fn parser_rejects_invalid_uid_target_and_mode() {
        for args_value in [
            &["--get", "--uid", "0", "--target", "sddm"][..],
            &["--get", "--uid", "01000", "--target", "sddm"][..],
            &["--get", "--uid", "1000", "--target", "kde"][..],
            &[
                "--set", "--uid", "1000", "--target", "sddm", "--mode", "enabled",
            ][..],
        ] {
            assert!(parse_args(args(args_value)).is_err());
        }
    }
}
