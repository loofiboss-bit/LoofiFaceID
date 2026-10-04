// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use kfaceauth_crypto_openssl_sys::{
    KEY_BYTES, current_uid, load_master_key_for_uid, seal_master_key, set_socket_permissions,
};
use kfaceauth_templates::{MasterKey, Vault, migrate_legacy_vault_with_separate_key};
use zeroize::Zeroize;

fn read_user_key_from_stdin() -> io::Result<[u8; KEY_BYTES]> {
    let mut key = [0_u8; KEY_BYTES];
    if let Err(error) = io::stdin().read_exact(&mut key) {
        key.zeroize();
        return Err(error);
    }
    let mut trailing = [0_u8; 1];
    let trailing_bytes = match io::stdin().read(&mut trailing) {
        Ok(bytes) => bytes,
        Err(error) => {
            key.zeroize();
            trailing.zeroize();
            return Err(error);
        }
    };
    if trailing_bytes != 0 {
        key.zeroize();
        trailing.zeroize();
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unexpected trailing key data",
        ));
    }
    trailing.zeroize();
    Ok(key)
}

fn usage() -> &'static str {
    "Usage: kfaceauth-sync-vault --uid UID --legacy-root DIR --enable-target sddm|plasma-lock | --uid UID --disable-target sddm|plasma-lock"
}

struct Options {
    target_uid: u32,
    legacy_root: Option<PathBuf>,
    system_root: Option<PathBuf>,
    keys_dir: Option<PathBuf>,
    target: String,
    enable: bool,
}

fn parse_args() -> Result<Option<Options>, ()> {
    let mut target_uid = current_uid();
    let mut legacy_root = None;
    let mut system_root = None;
    let mut keys_dir = None;
    let mut enable_target: Option<String> = None;
    let mut disable_target: Option<String> = None;

    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--uid" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --uid");
                    return Err(());
                };
                let Ok(uid) = value.parse::<u32>() else {
                    eprintln!("Invalid UID");
                    return Err(());
                };
                target_uid = uid;
            }
            "--legacy-root" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --legacy-root");
                    return Err(());
                };
                legacy_root = Some(PathBuf::from(value));
            }
            "--system-root" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --system-root");
                    return Err(());
                };
                system_root = Some(PathBuf::from(value));
            }
            "--keys-dir" => {
                let Some(value) = args.next() else {
                    eprintln!("Missing value for --keys-dir");
                    return Err(());
                };
                keys_dir = Some(PathBuf::from(value));
            }
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

    Ok(Some(Options {
        target_uid,
        legacy_root,
        system_root,
        keys_dir,
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

fn user_session_key() -> Option<MasterKey> {
    let Ok(mut key_bytes) = read_user_key_from_stdin() else {
        eprintln!("A 32-byte user-session key is required on standard input");
        return None;
    };
    let key = MasterKey::from_bytes(key_bytes);
    key_bytes.zeroize();
    Some(key)
}

fn load_or_generate_system_key(target_uid: u32, keys_dir: &Path) -> Option<(MasterKey, bool)> {
    let key_file = keys_dir.join(format!("{target_uid}.key"));
    match fs::symlink_metadata(&key_file) {
        Ok(_) => {
            let Ok(key) = load_master_key_for_uid(target_uid, Some(keys_dir)) else {
                eprintln!("The existing system-login key is unreadable; refusing to replace it");
                return None;
            };
            Some((MasterKey::from_bytes(key), false))
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
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

fn locate_legacy_vault(target_uid: u32, custom_root: Option<&Path>) -> Option<Vault> {
    if let Some(root) = custom_root {
        return Some(Vault::user_session_with_root(root, target_uid));
    }

    let Ok(passwd) = fs::read_to_string("/etc/passwd") else {
        eprintln!("Could not locate the user's local profile");
        return None;
    };
    for line in passwd.lines() {
        let fields: Vec<&str> = line.split(':').collect();
        if fields.len() >= 6 && fields[2].parse::<u32>().ok() == Some(target_uid) {
            let root = PathBuf::from(fields[5])
                .join(".local/share")
                .join(kfaceauth_templates::PRODUCT_DIRECTORY);
            if root.exists() {
                return Some(Vault::user_session_with_root(&root, target_uid));
            }
        }
    }
    eprintln!("Could not locate the user's local profile");
    None
}

fn apply_system_profile_permissions(
    target_uid: u32,
    system_vault_base: &Path,
    keys_dir: &Path,
) -> bool {
    let key_file = keys_dir.join(format!("{target_uid}.key"));
    let vault_dir = system_vault_base.join(target_uid.to_string());
    let vault_file = vault_dir.join("identity.vault");
    let permissions_ok = (|| {
        if let Some(parent) = keys_dir.parent() {
            set_socket_permissions(parent, 0o755, Some("kfaceauth"))?;
        }
        set_socket_permissions(keys_dir, 0o750, Some("kfaceauth"))?;
        set_socket_permissions(&key_file, 0o640, Some("kfaceauth"))?;
        set_socket_permissions(system_vault_base, 0o755, Some("kfaceauth"))?;
        std::os::unix::fs::chown(&vault_dir, Some(target_uid), None)?;
        set_socket_permissions(&vault_dir, 0o750, Some("kfaceauth"))?;
        std::os::unix::fs::chown(&vault_file, Some(target_uid), None)?;
        set_socket_permissions(&vault_file, 0o640, Some("kfaceauth"))?;
        Ok::<(), Box<dyn std::error::Error>>(())
    })();
    if permissions_ok.is_err() {
        eprintln!("Could not apply system-login profile permissions");
        return false;
    }
    true
}

fn enable_target(options: Options) -> ExitCode {
    let target_uid = options.target_uid;
    let target = options.target;
    let system_vault_base = options
        .system_root
        .unwrap_or_else(|| PathBuf::from("/var/lib/kfaceauth"));
    let keys_dir = options
        .keys_dir
        .unwrap_or_else(|| PathBuf::from("/etc/kfaceauth/keys"));

    let Some(user_key) = user_session_key() else {
        return ExitCode::FAILURE;
    };
    let Some((system_key, new_system_key)) = load_or_generate_system_key(target_uid, &keys_dir)
    else {
        return ExitCode::FAILURE;
    };
    if new_system_key
        && seal_master_key(target_uid, system_key.sensitive_bytes(), Some(&keys_dir)).is_err()
    {
        eprintln!("Could not store the system-login key");
        return ExitCode::FAILURE;
    }

    let Some(legacy_vault) = locate_legacy_vault(target_uid, options.legacy_root.as_deref()) else {
        return ExitCode::FAILURE;
    };
    let system_vault = Vault::system_with_root(&system_vault_base, target_uid);
    let Ok(summary) = migrate_legacy_vault_with_separate_key(
        &legacy_vault,
        &system_vault,
        &user_key,
        &system_key,
    ) else {
        eprintln!("Could not validate and provision the system-login profile");
        return ExitCode::FAILURE;
    };

    if !apply_system_profile_permissions(target_uid, &system_vault_base, &keys_dir) {
        return ExitCode::FAILURE;
    }
    if !run_setup(&target, target_uid, true) {
        eprintln!("The profile is provisioned, but target activation failed");
        return ExitCode::FAILURE;
    }

    println!("result=ok target={target} samples={}", summary.sample_count);
    ExitCode::SUCCESS
}

fn main() -> ExitCode {
    let options = match parse_args() {
        Ok(Some(options)) => options,
        Ok(None) => return ExitCode::SUCCESS,
        Err(()) => return ExitCode::FAILURE,
    };
    if !options.enable {
        if run_setup(&options.target, options.target_uid, false) {
            println!("target={} state=disabled", options.target);
            return ExitCode::SUCCESS;
        }
        eprintln!("Could not disable the selected authentication target");
        return ExitCode::FAILURE;
    }
    enable_target(options)
}
